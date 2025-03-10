/* SPDX-License-Identifier: LGPL-2.1+ */

#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>

#if HAVE_CRYPT_H
/* libxcrypt is a replacement for glibc's libcrypt, and libcrypt might be
 * removed from glibc at some point. As part of the removal, defines for
 * crypt(3) are dropped from unistd.h, and we must include crypt.h instead.
 *
 * Newer versions of glibc (v2.0+) already ship crypt.h with a definition
 * of crypt(3) as well, so we simply include it if it is present.  MariaDB,
 * MySQL, PostgreSQL, Perl and some other wide-spread packages do it the
 * same way since ages without any problems.
 */
#  include <crypt.h>
#endif

#include "sd-id128.h"

#include "alloc-util.h"
#include "ask-password-api.h"
#include "copy.h"
#include "env-file.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "hostname-util.h"
#include "kbd-util.h"
#include "locale-util.h"
#include "main-func.h"
#include "memory-util.h"
#include "mkdir.h"
#include "os-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "pretty-print.h"
#include "proc-cmdline.h"
#include "random-util.h"
#include "string-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "time-util.h"
#include "umask-util.h"
#include "user-util.h"

static char *arg_root = NULL;
static char *arg_locale = NULL;  /* $LANG */
static char *arg_keymap = NULL;
static char *arg_locale_messages = NULL; /* $LC_MESSAGES */
static char *arg_timezone = NULL;
static char *arg_hostname = NULL;
static sd_id128_t arg_machine_id = {};
static char *arg_root_password = NULL;
static bool arg_prompt_locale = false;
static bool arg_prompt_keymap = false;
static bool arg_prompt_timezone = false;
static bool arg_prompt_hostname = false;
static bool arg_prompt_root_password = false;
static bool arg_copy_locale = false;
static bool arg_copy_keymap = false;
static bool arg_copy_timezone = false;
static bool arg_copy_root_password = false;

STATIC_DESTRUCTOR_REGISTER(arg_root, freep);
STATIC_DESTRUCTOR_REGISTER(arg_locale, freep);
STATIC_DESTRUCTOR_REGISTER(arg_locale_messages, freep);
STATIC_DESTRUCTOR_REGISTER(arg_keymap, freep);
STATIC_DESTRUCTOR_REGISTER(arg_timezone, freep);
STATIC_DESTRUCTOR_REGISTER(arg_hostname, freep);
STATIC_DESTRUCTOR_REGISTER(arg_root_password, erase_and_freep);

static bool press_any_key(void) {
        char k = 0;
        bool need_nl = true;

        printf("-- Press any key to proceed --");
        fflush(stdout);

        (void) read_one_char(stdin, &k, USEC_INFINITY, &need_nl);

        if (need_nl)
                putchar('\n');

        return k != 'q';
}

static void print_welcome(void) {
        _cleanup_free_ char *pretty_name = NULL, *ansi_color = NULL;
        static bool done = false;
        const char *pn;
        int r;

        if (done)
                return;

        r = parse_os_release(
                        arg_root,
                        "PRETTY_NAME", &pretty_name,
                        "ANSI_COLOR", &ansi_color,
                        NULL);
        if (r < 0)
                log_full_errno(r == -ENOENT ? LOG_DEBUG : LOG_WARNING, r,
                               "Failed to read os-release file, ignoring: %m");

        pn = isempty(pretty_name) ? "Linux" : pretty_name;

        if (colors_enabled())
                printf("\nWelcome to your new installation of \x1B[%sm%s\x1B[0m!\n", ansi_color, pn);
        else
                printf("\nWelcome to your new installation of %s!\n", pn);

        printf("\nPlease configure your system!\n\n");

        press_any_key();

        done = true;
}

static int show_menu(char **x, unsigned n_columns, unsigned width, unsigned percentage) {
        unsigned break_lines, break_modulo;
        size_t n, per_column, i, j;

        assert(n_columns > 0);

        n = strv_length(x);
        per_column = DIV_ROUND_UP(n, n_columns);

        break_lines = lines();
        if (break_lines > 2)
                break_lines--;

        /* The first page gets two extra lines, since we want to show
         * a title */
        break_modulo = break_lines;
        if (break_modulo > 3)
                break_modulo -= 3;

        for (i = 0; i < per_column; i++) {

                for (j = 0; j < n_columns; j ++) {
                        _cleanup_free_ char *e = NULL;

                        if (j * per_column + i >= n)
                                break;

                        e = ellipsize(x[j * per_column + i], width, percentage);
                        if (!e)
                                return log_oom();

                        printf("%4zu) %-*s", j * per_column + i + 1, width, e);
                }

                putchar('\n');

                /* on the first screen we reserve 2 extra lines for the title */
                if (i % break_lines == break_modulo) {
                        if (!press_any_key())
                                return 0;
                }
        }

        return 0;
}

static int prompt_loop(const char *text, char **l, bool (*is_valid)(const char *name), char **ret) {
        int r;

        assert(text);
        assert(is_valid);
        assert(ret);

        for (;;) {
                _cleanup_free_ char *p = NULL;
                unsigned u;

                r = ask_string(&p, "%s %s (empty to skip): ", special_glyph(SPECIAL_GLYPH_TRIANGULAR_BULLET), text);
                if (r < 0)
                        return log_error_errno(r, "Failed to query user: %m");

                if (isempty(p)) {
                        log_warning("No data entered, skipping.");
                        return 0;
                }

                r = safe_atou(p, &u);
                if (r >= 0) {
                        char *c;

                        if (u <= 0 || u > strv_length(l)) {
                                log_error("Specified entry number out of range.");
                                continue;
                        }

                        log_info("Selected '%s'.", l[u-1]);

                        c = strdup(l[u-1]);
                        if (!c)
                                return log_oom();

                        free(*ret);
                        *ret = c;
                        return 0;
                }

                if (!is_valid(p)) {
                        log_error("Entered data invalid.");
                        continue;
                }

                free(*ret);
                *ret = p;
                p = 0;
                return 0;
        }
}

static int prompt_locale(void) {
        _cleanup_strv_free_ char **locales = NULL;
        int r;

        if (arg_locale || arg_locale_messages)
                return 0;

        if (!arg_prompt_locale)
                return 0;

        r = get_locales(&locales);
        if (r < 0)
                return log_error_errno(r, "Cannot query locales list: %m");

        if (strv_isempty(locales))
                log_debug("No locales found, skipping locale selection.");
        else if (strv_length(locales) == 1) {

                if (streq(locales[0], SYSTEMD_DEFAULT_LOCALE))
                        log_debug("Only installed locale is default locale anyway, not setting locale explicitly.");
                else {
                        log_debug("Only a single locale available (%s), selecting it as default.", locales[0]);

                        arg_locale = strdup(locales[0]);
                        if (!arg_locale)
                                return log_oom();

                        /* Not setting arg_locale_message here, since it defaults to LANG anyway */
                }
        } else {
                print_welcome();

                printf("\nAvailable Locales:\n\n");
                r = show_menu(locales, 3, 22, 60);
                if (r < 0)
                        return r;

                putchar('\n');

                r = prompt_loop("Please enter system locale name or number", locales, locale_is_valid, &arg_locale);
                if (r < 0)
                        return r;

                if (isempty(arg_locale))
                        return 0;

                r = prompt_loop("Please enter system message locale name or number", locales, locale_is_valid, &arg_locale_messages);
                if (r < 0)
                        return r;

                /* Suppress the messages setting if it's the same as the main locale anyway */
                if (streq_ptr(arg_locale, arg_locale_messages))
                        arg_locale_messages = mfree(arg_locale_messages);
        }

        return 0;
}

static int process_locale(void) {
        const char *etc_localeconf;
        char* locales[3];
        unsigned i = 0;
        int r;

        etc_localeconf = prefix_roota(arg_root, "/etc/locale.conf");
        if (laccess(etc_localeconf, F_OK) >= 0)
                return 0;

        if (arg_copy_locale && arg_root) {

                (void) mkdir_parents(etc_localeconf, 0755);
                r = copy_file("/etc/locale.conf", etc_localeconf, 0, 0644, 0, 0, COPY_REFLINK);
                if (r != -ENOENT) {
                        if (r < 0)
                                return log_error_errno(r, "Failed to copy %s: %m", etc_localeconf);

                        log_info("%s copied.", etc_localeconf);
                        return 0;
                }
        }

        r = prompt_locale();
        if (r < 0)
                return r;

        if (!isempty(arg_locale))
                locales[i++] = strjoina("LANG=", arg_locale);
        if (!isempty(arg_locale_messages) && !streq(arg_locale_messages, arg_locale))
                locales[i++] = strjoina("LC_MESSAGES=", arg_locale_messages);

        if (i == 0)
                return 0;

        locales[i] = NULL;

        (void) mkdir_parents(etc_localeconf, 0755);
        r = write_env_file(etc_localeconf, locales);
        if (r < 0)
                return log_error_errno(r, "Failed to write %s: %m", etc_localeconf);

        log_info("%s written.", etc_localeconf);
        return 0;
}

static int prompt_keymap(void) {
        _cleanup_strv_free_ char **kmaps = NULL;
        int r;

        if (arg_keymap)
                return 0;

        if (!arg_prompt_keymap)
                return 0;

        r = get_keymaps(&kmaps);
        if (r == -ENOENT) /* no keymaps installed */
                return r;
        if (r < 0)
                return log_error_errno(r, "Failed to read keymaps: %m");

        print_welcome();

        printf("\nAvailable keymaps:\n\n");
        r = show_menu(kmaps, 3, 22, 60);
        if (r < 0)
                return r;

        putchar('\n');

        return prompt_loop("Please enter system keymap name or number",
                           kmaps, keymap_is_valid, &arg_keymap);
}

static int process_keymap(void) {
        const char *etc_vconsoleconf;
        char **keymap;
        int r;

        etc_vconsoleconf = prefix_roota(arg_root, "/etc/vconsole.conf");
        if (laccess(etc_vconsoleconf, F_OK) >= 0)
                return 0;

        if (arg_copy_keymap && arg_root) {

                (void) mkdir_parents(etc_vconsoleconf, 0755);
                r = copy_file("/etc/vconsole.conf", etc_vconsoleconf, 0, 0644, 0, 0, COPY_REFLINK);
                if (r != -ENOENT) {
                        if (r < 0)
                                return log_error_errno(r, "Failed to copy %s: %m", etc_vconsoleconf);

                        log_info("%s copied.", etc_vconsoleconf);
                        return 0;
                }
        }

        r = prompt_keymap();
        if (r == -ENOENT)
                return 0; /* don't fail if no keymaps are installed */
        if (r < 0)
                return r;

        if (isempty(arg_keymap))
                return 0;

        keymap = STRV_MAKE(strjoina("KEYMAP=", arg_keymap));

        r = mkdir_parents(etc_vconsoleconf, 0755);
        if (r < 0)
                return log_error_errno(r, "Failed to create the parent directory of %s: %m", etc_vconsoleconf);

        r = write_env_file(etc_vconsoleconf, keymap);
        if (r < 0)
                return log_error_errno(r, "Failed to write %s: %m", etc_vconsoleconf);

        log_info("%s written.", etc_vconsoleconf);
        return 0;
}

static bool timezone_is_valid_log_error(const char *name) {
        return timezone_is_valid(name, LOG_ERR);
}

static int prompt_timezone(void) {
        _cleanup_strv_free_ char **zones = NULL;
        int r;

        if (arg_timezone)
                return 0;

        if (!arg_prompt_timezone)
                return 0;

        r = get_timezones(&zones);
        if (r < 0)
                return log_error_errno(r, "Cannot query timezone list: %m");

        print_welcome();

        printf("\nAvailable Time Zones:\n\n");
        r = show_menu(zones, 3, 22, 30);
        if (r < 0)
                return r;

        putchar('\n');

        r = prompt_loop("Please enter timezone name or number", zones, timezone_is_valid_log_error, &arg_timezone);
        if (r < 0)
                return r;

        return 0;
}

static int process_timezone(void) {
        const char *etc_localtime, *e;
        int r;

        etc_localtime = prefix_roota(arg_root, "/etc/localtime");
        if (laccess(etc_localtime, F_OK) >= 0)
                return 0;

        if (arg_copy_timezone && arg_root) {
                _cleanup_free_ char *p = NULL;

                r = readlink_malloc("/etc/localtime", &p);
                if (r != -ENOENT) {
                        if (r < 0)
                                return log_error_errno(r, "Failed to read host timezone: %m");

                        (void) mkdir_parents(etc_localtime, 0755);
                        if (symlink(p, etc_localtime) < 0)
                                return log_error_errno(errno, "Failed to create %s symlink: %m", etc_localtime);

                        log_info("%s copied.", etc_localtime);
                        return 0;
                }
        }

        r = prompt_timezone();
        if (r < 0)
                return r;

        if (isempty(arg_timezone))
                return 0;

        e = strjoina("../usr/share/zoneinfo/", arg_timezone);

        (void) mkdir_parents(etc_localtime, 0755);
        if (symlink(e, etc_localtime) < 0)
                return log_error_errno(errno, "Failed to create %s symlink: %m", etc_localtime);

        log_info("%s written", etc_localtime);
        return 0;
}

static int prompt_hostname(void) {
        int r;

        if (arg_hostname)
                return 0;

        if (!arg_prompt_hostname)
                return 0;

        print_welcome();
        putchar('\n');

        for (;;) {
                _cleanup_free_ char *h = NULL;

                r = ask_string(&h, "%s Please enter hostname for new system (empty to skip): ", special_glyph(SPECIAL_GLYPH_TRIANGULAR_BULLET));
                if (r < 0)
                        return log_error_errno(r, "Failed to query hostname: %m");

                if (isempty(h)) {
                        log_warning("No hostname entered, skipping.");
                        break;
                }

                if (!hostname_is_valid(h, true)) {
                        log_error("Specified hostname invalid.");
                        continue;
                }

                /* Get rid of the trailing dot that we allow, but don't want to see */
                arg_hostname = hostname_cleanup(h);
                h = NULL;
                break;
        }

        return 0;
}

static int process_hostname(void) {
        const char *etc_hostname;
        int r;

        etc_hostname = prefix_roota(arg_root, "/etc/hostname");
        if (laccess(etc_hostname, F_OK) >= 0)
                return 0;

        r = prompt_hostname();
        if (r < 0)
                return r;

        if (isempty(arg_hostname))
                return 0;

        r = write_string_file(etc_hostname, arg_hostname,
                              WRITE_STRING_FILE_CREATE | WRITE_STRING_FILE_SYNC | WRITE_STRING_FILE_MKDIR_0755);
        if (r < 0)
                return log_error_errno(r, "Failed to write %s: %m", etc_hostname);

        log_info("%s written.", etc_hostname);
        return 0;
}

static int process_machine_id(void) {
        const char *etc_machine_id;
        char id[SD_ID128_STRING_MAX];
        int r;

        etc_machine_id = prefix_roota(arg_root, "/etc/machine-id");
        if (laccess(etc_machine_id, F_OK) >= 0)
                return 0;

        if (sd_id128_is_null(arg_machine_id))
                return 0;

        r = write_string_file(etc_machine_id, sd_id128_to_string(arg_machine_id, id),
                              WRITE_STRING_FILE_CREATE | WRITE_STRING_FILE_SYNC | WRITE_STRING_FILE_MKDIR_0755);
        if (r < 0)
                return log_error_errno(r, "Failed to write machine id: %m");

        log_info("%s written.", etc_machine_id);
        return 0;
}

static int prompt_root_password(void) {
        const char *msg1, *msg2, *etc_shadow;
        int r;

        if (arg_root_password)
                return 0;

        if (!arg_prompt_root_password)
                return 0;

        etc_shadow = prefix_roota(arg_root, "/etc/shadow");
        if (laccess(etc_shadow, F_OK) >= 0)
                return 0;

        print_welcome();
        putchar('\n');

        msg1 = strjoina(special_glyph(SPECIAL_GLYPH_TRIANGULAR_BULLET), " Please enter a new root password (empty to skip): ");
        msg2 = strjoina(special_glyph(SPECIAL_GLYPH_TRIANGULAR_BULLET), " Please enter new root password again: ");

        for (;;) {
                _cleanup_strv_free_erase_ char **a = NULL, **b = NULL;

                r = ask_password_tty(-1, msg1, NULL, 0, 0, NULL, &a);
                if (r < 0)
                        return log_error_errno(r, "Failed to query root password: %m");
                if (strv_length(a) != 1) {
                        log_warning("Received multiple passwords, where we expected one.");
                        return -EINVAL;
                }

                if (isempty(*a)) {
                        log_warning("No password entered, skipping.");
                        break;
                }

                r = ask_password_tty(-1, msg2, NULL, 0, 0, NULL, &b);
                if (r < 0)
                        return log_error_errno(r, "Failed to query root password: %m");

                if (!streq(*a, *b)) {
                        log_error("Entered passwords did not match, please try again.");
                        continue;
                }

                arg_root_password = TAKE_PTR(*a);
                break;
        }

        return 0;
}

static int write_root_shadow(const char *path, const struct spwd *p) {
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(path);
        assert(p);

        RUN_WITH_UMASK(0777)
                f = fopen(path, "wex");
        if (!f)
                return -errno;

        r = putspent_sane(p, f);
        if (r < 0)
                return r;

        return fflush_sync_and_check(f);
}

static int process_root_password(void) {

        struct spwd item = {
                .sp_namp = (char*) "root",
                .sp_min = -1,
                .sp_max = -1,
                .sp_warn = -1,
                .sp_inact = -1,
                .sp_expire = -1,
                .sp_flag = (unsigned long) -1, /* this appears to be what everybody does ... */
        };
        _cleanup_free_ char *salt = NULL;
        _cleanup_close_ int lock = -1;
        struct crypt_data cd = {};

        const char *etc_shadow;
        int r;

        etc_shadow = prefix_roota(arg_root, "/etc/shadow");
        if (laccess(etc_shadow, F_OK) >= 0)
                return 0;

        (void) mkdir_parents(etc_shadow, 0755);

        lock = take_etc_passwd_lock(arg_root);
        if (lock < 0)
                return log_error_errno(lock, "Failed to take a lock: %m");

        if (arg_copy_root_password && arg_root) {
                struct spwd *p;

                errno = 0;
                p = getspnam("root");
                if (p || errno != ENOENT) {
                        if (!p) {
                                if (!errno)
                                        errno = EIO;

                                return log_error_errno(errno, "Failed to find shadow entry for root: %m");
                        }

                        r = write_root_shadow(etc_shadow, p);
                        if (r < 0)
                                return log_error_errno(r, "Failed to write %s: %m", etc_shadow);

                        log_info("%s copied.", etc_shadow);
                        return 0;
                }
        }

        r = prompt_root_password();
        if (r < 0)
                return r;

        if (!arg_root_password)
                return 0;

        r = make_salt(&salt);
        if (r < 0)
                return log_error_errno(r, "Failed to get salt: %m");

        errno = 0;
        item.sp_pwdp = crypt_r(arg_root_password, salt, &cd);
        if (!item.sp_pwdp)
                return log_error_errno(errno == 0 ? SYNTHETIC_ERRNO(EINVAL) : errno,
                                       "Failed to encrypt password: %m");

        item.sp_lstchg = (long) (now(CLOCK_REALTIME) / USEC_PER_DAY);

        r = write_root_shadow(etc_shadow, &item);
        if (r < 0)
                return log_error_errno(r, "Failed to write %s: %m", etc_shadow);

        log_info("%s written.", etc_shadow);
        return 0;
}

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-firstboot", "1", &link);
        if (r < 0)
                return log_oom();

        printf("%s [OPTIONS...]\n\n"
               "Configures basic settings of the system.\n\n"
               "  -h --help                    Show this help\n"
               "     --version                 Show package version\n"
               "     --root=PATH               Operate on an alternate filesystem root\n"
               "     --locale=LOCALE           Set primary locale (LANG=)\n"
               "     --locale-messages=LOCALE  Set message locale (LC_MESSAGES=)\n"
               "     --keymap=KEYMAP           Set keymap\n"
               "     --timezone=TIMEZONE       Set timezone\n"
               "     --hostname=NAME           Set host name\n"
               "     --machine-ID=ID           Set machine ID\n"
               "     --root-password=PASSWORD  Set root password\n"
               "     --root-password-file=FILE Set root password from file\n"
               "     --prompt-locale           Prompt the user for locale settings\n"
               "     --prompt-keymap           Prompt the user for keymap settings\n"
               "     --prompt-timezone         Prompt the user for timezone\n"
               "     --prompt-hostname         Prompt the user for hostname\n"
               "     --prompt-root-password    Prompt the user for root password\n"
               "     --prompt                  Prompt for all of the above\n"
               "     --copy-locale             Copy locale from host\n"
               "     --copy-keymap             Copy keymap from host\n"
               "     --copy-timezone           Copy timezone from host\n"
               "     --copy-root-password      Copy root password from host\n"
               "     --copy                    Copy locale, keymap, timezone, root password\n"
               "     --setup-machine-id        Generate a new random machine ID\n"
               "\nSee the %s for details.\n"
               , program_invocation_short_name
               , link
        );

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_ROOT,
                ARG_LOCALE,
                ARG_LOCALE_MESSAGES,
                ARG_KEYMAP,
                ARG_TIMEZONE,
                ARG_HOSTNAME,
                ARG_MACHINE_ID,
                ARG_ROOT_PASSWORD,
                ARG_ROOT_PASSWORD_FILE,
                ARG_PROMPT,
                ARG_PROMPT_LOCALE,
                ARG_PROMPT_KEYMAP,
                ARG_PROMPT_TIMEZONE,
                ARG_PROMPT_HOSTNAME,
                ARG_PROMPT_ROOT_PASSWORD,
                ARG_COPY,
                ARG_COPY_LOCALE,
                ARG_COPY_KEYMAP,
                ARG_COPY_TIMEZONE,
                ARG_COPY_ROOT_PASSWORD,
                ARG_SETUP_MACHINE_ID,
        };

        static const struct option options[] = {
                { "help",                 no_argument,       NULL, 'h'                      },
                { "version",              no_argument,       NULL, ARG_VERSION              },
                { "root",                 required_argument, NULL, ARG_ROOT                 },
                { "locale",               required_argument, NULL, ARG_LOCALE               },
                { "locale-messages",      required_argument, NULL, ARG_LOCALE_MESSAGES      },
                { "keymap",               required_argument, NULL, ARG_KEYMAP               },
                { "timezone",             required_argument, NULL, ARG_TIMEZONE             },
                { "hostname",             required_argument, NULL, ARG_HOSTNAME             },
                { "machine-id",           required_argument, NULL, ARG_MACHINE_ID           },
                { "root-password",        required_argument, NULL, ARG_ROOT_PASSWORD        },
                { "root-password-file",   required_argument, NULL, ARG_ROOT_PASSWORD_FILE   },
                { "prompt",               no_argument,       NULL, ARG_PROMPT               },
                { "prompt-locale",        no_argument,       NULL, ARG_PROMPT_LOCALE        },
                { "prompt-keymap",        no_argument,       NULL, ARG_PROMPT_KEYMAP        },
                { "prompt-timezone",      no_argument,       NULL, ARG_PROMPT_TIMEZONE      },
                { "prompt-hostname",      no_argument,       NULL, ARG_PROMPT_HOSTNAME      },
                { "prompt-root-password", no_argument,       NULL, ARG_PROMPT_ROOT_PASSWORD },
                { "copy",                 no_argument,       NULL, ARG_COPY                 },
                { "copy-locale",          no_argument,       NULL, ARG_COPY_LOCALE          },
                { "copy-keymap",          no_argument,       NULL, ARG_COPY_KEYMAP          },
                { "copy-timezone",        no_argument,       NULL, ARG_COPY_TIMEZONE        },
                { "copy-root-password",   no_argument,       NULL, ARG_COPY_ROOT_PASSWORD   },
                { "setup-machine-id",     no_argument,       NULL, ARG_SETUP_MACHINE_ID     },
                {}
        };

        int r, c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case ARG_ROOT:
                        r = parse_path_argument_and_warn(optarg, true, &arg_root);
                        if (r < 0)
                                return r;
                        break;

                case ARG_LOCALE:
                        if (!locale_is_valid(optarg))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Locale %s is not valid.", optarg);

                        r = free_and_strdup(&arg_locale, optarg);
                        if (r < 0)
                                return log_oom();

                        break;

                case ARG_LOCALE_MESSAGES:
                        if (!locale_is_valid(optarg))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Locale %s is not valid.", optarg);

                        r = free_and_strdup(&arg_locale_messages, optarg);
                        if (r < 0)
                                return log_oom();

                        break;

                case ARG_KEYMAP:
                        if (!keymap_is_valid(optarg))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Keymap %s is not valid.", optarg);

                        r = free_and_strdup(&arg_keymap, optarg);
                        if (r < 0)
                                return log_oom();

                        break;

                case ARG_TIMEZONE:
                        if (!timezone_is_valid(optarg, LOG_ERR))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Timezone %s is not valid.", optarg);

                        r = free_and_strdup(&arg_timezone, optarg);
                        if (r < 0)
                                return log_oom();

                        break;

                case ARG_ROOT_PASSWORD:
                        r = free_and_strdup(&arg_root_password, optarg);
                        if (r < 0)
                                return log_oom();
                        break;

                case ARG_ROOT_PASSWORD_FILE:
                        arg_root_password = mfree(arg_root_password);

                        r = read_one_line_file(optarg, &arg_root_password);
                        if (r < 0)
                                return log_error_errno(r, "Failed to read %s: %m", optarg);

                        break;

                case ARG_HOSTNAME:
                        if (!hostname_is_valid(optarg, true))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Host name %s is not valid.", optarg);

                        hostname_cleanup(optarg);
                        r = free_and_strdup(&arg_hostname, optarg);
                        if (r < 0)
                                return log_oom();

                        break;

                case ARG_MACHINE_ID:
                        if (sd_id128_from_string(optarg, &arg_machine_id) < 0)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Failed to parse machine id %s.", optarg);

                        break;

                case ARG_PROMPT:
                        arg_prompt_locale = arg_prompt_keymap = arg_prompt_timezone = arg_prompt_hostname = arg_prompt_root_password = true;
                        break;

                case ARG_PROMPT_LOCALE:
                        arg_prompt_locale = true;
                        break;

                case ARG_PROMPT_KEYMAP:
                        arg_prompt_keymap = true;
                        break;

                case ARG_PROMPT_TIMEZONE:
                        arg_prompt_timezone = true;
                        break;

                case ARG_PROMPT_HOSTNAME:
                        arg_prompt_hostname = true;
                        break;

                case ARG_PROMPT_ROOT_PASSWORD:
                        arg_prompt_root_password = true;
                        break;

                case ARG_COPY:
                        arg_copy_locale = arg_copy_keymap = arg_copy_timezone = arg_copy_root_password = true;
                        break;

                case ARG_COPY_LOCALE:
                        arg_copy_locale = true;
                        break;

                case ARG_COPY_KEYMAP:
                        arg_copy_keymap = true;
                        break;

                case ARG_COPY_TIMEZONE:
                        arg_copy_timezone = true;
                        break;

                case ARG_COPY_ROOT_PASSWORD:
                        arg_copy_root_password = true;
                        break;

                case ARG_SETUP_MACHINE_ID:

                        r = sd_id128_randomize(&arg_machine_id);
                        if (r < 0)
                                return log_error_errno(r, "Failed to generate randomized machine ID: %m");

                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        return 1;
}

static int run(int argc, char *argv[]) {
        bool enabled;
        int r;

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        log_setup_service();

        umask(0022);

        r = proc_cmdline_get_bool("systemd.firstboot", &enabled);
        if (r < 0)
                return log_error_errno(r, "Failed to parse systemd.firstboot= kernel command line argument, ignoring: %m");
        if (r > 0 && !enabled)
                return 0; /* disabled */

        r = process_locale();
        if (r < 0)
                return r;

        r = process_keymap();
        if (r < 0)
                return r;

        r = process_timezone();
        if (r < 0)
                return r;

        r = process_hostname();
        if (r < 0)
                return r;

        r = process_machine_id();
        if (r < 0)
                return r;

        r = process_root_password();
        if (r < 0)
                return r;

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
