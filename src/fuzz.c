/** @file fuzz.c */
/*
 * dfuzzer - tool for fuzz testing processes communicating through D-Bus.
 *
 * Copyright(C) 2013, Red Hat, Inc., Matus Marhefka <mmarhefk@redhat.com>
 *                                   Miroslav Vadkerti <mvadkert@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ffi.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fuzz.h"
#include "dfuzzer.h"
#include "rand.h"
#include "util.h"


/** Pointer on D-Bus interface proxy for calling methods. */
static GDBusProxy *df_dproxy;
/** Structure containing information about the linked list. */
static struct df_sig_list df_list;
/** Pointer on the last item of the linked list in the global var. df_list. */
static struct df_signature *df_last;
/** Flag for unsupported method signature, 1 means signature is unsupported */
static int df_unsupported_sig;
/** Pointer on unsupported signature string (do not free it) */
static char *df_unsupported_sig_str;
/** Exceptions counter; if MAX_EXCEPTIONS is reached testing continues
  * with a next method */
static char df_except_counter = 0;


/* Module static functions */
static int df_fuzz_write_log(void);
static int df_exec_cmd_check(const char *cmd);
static GVariant *df_fuzz_create_variant(void);
static int df_fuzz_create_list_variants(void);
static int df_fuzz_create_fmt_string(char **fmt, const int n);
static int df_fuzz_call_method(GVariant *value, const int void_method);


/**
 * @function Saves pointer on D-Bus interface proxy for this module to be
 * able to call methods through this proxy during fuzz testing.
 * @param dproxy Pointer on D-Bus interface proxy
 * @return 0 on success, -1 on error
 */

int df_fuzz_init(GDBusProxy *dproxy)
{
        if (dproxy == NULL) {
                df_debug("Passing NULL argument to function.\n");
                return -1;
        }
        df_dproxy = dproxy;

        return 0;
}

/**
 * @function Initializes the global variable df_list (struct df_sig_list)
 * including allocationg memory for method name inside df_list.
 * @param name Name of method which will be tested
 * @return 0 on success, -1 on error
 */
int df_fuzz_add_method(const char *name)
{
        if (!name) {
                df_debug("Passing NULL argument to function.\n");
                return -1;
        }

        df_list.df_method_name = strdup(name);
        if (!df_list.df_method_name) {
                df_fail("Error: Could not allocate memory for method name.\n");
                return -1;
        }

        // must be initialized because after df_fuzz_clean_method() memory
        // of df_list contains junk
        df_list.list = NULL;    // no arguments so far
        df_list.args = 0;
        df_list.fuzz_on_str_len = 0;

        return 0;
}

/**
 * @function Adds item (struct df_signature) at the end of the linked list
 * in the global variable df_list (struct df_sig_list). This includes
 * allocating memory for item and for signature string.
 * @param signature D-Bus signature of the argument
 * @return 0 on success, -1 on error
 */
int df_fuzz_add_method_arg(const char *signature)
{
        struct df_signature *s;

        if (!signature)
                return 0;

        s = malloc(sizeof(*s));
        if (!s) {
                df_fail("Error: Could not allocate memory for struct df_signature.\n");
                return -1;
        }

        df_list.args++;
        s->next = NULL;
        s->var = NULL;
        s->sig = strdup(signature);
        if (!s->sig) {
                df_fail("Error: Could not allocate memory for argument signature.\n");
                free(s);
                return -1;
        }

        // fuzzing controlled by generated random strings lengths
        if (strstr(s->sig, "s") || strstr(s->sig, "v"))
                df_list.fuzz_on_str_len = 1;

        if (!df_list.list) {
                df_list.list = s;
                df_last = s;
        } else {
                df_last->next = s;
                df_last = s;
        }

        return 0;
}

/**
 * @return Number of arguments of tested method
 */
int df_list_args_count(void)
{
        return df_list.args;
}

/**
 * @function Prints all method signatures and their values on the output.
 * @return 0 on success, -1 on error
 */
static int df_fuzz_write_log(void)
{
        struct df_signature *s = df_list.list;  // pointer on first signature
        int len = 0;

        FULL_LOG("%s;", df_list.df_method_name);

        while (s != NULL) {
                len = strlen(s->sig);
                if (len <= 0) {
                        df_fail("No argument signature\n");
                        FULL_LOG("\n");
                        return -1;
                } else if (len == 1) {  // one character argument
                        df_fail("    --");
                        df_fail("%s", s->sig);
                        FULL_LOG("%s;", s->sig);

                        switch (s->sig[0]) {
                                case 'y': {
                                        guint8 u;

                                        g_variant_get(s->var, s->sig, &u);
                                        df_fail("-- '%u'\n", u);
                                        FULL_LOG("%u;", u);
                                        break;
                                }
                                case 'b': {
                                        gboolean b;

                                        g_variant_get(s->var, s->sig, &b);
                                        df_fail("-- '%s'\n", b ? "true" : "false");
                                        FULL_LOG("%s", b ? "true" : "false");
                                        break;
                                }
                                case 'n': {
                                        gint16 i;

                                        g_variant_get(s->var, s->sig, &i);
                                        df_fail("-- '%d'\n", i);
                                        FULL_LOG("%d;", i);
                                        break;
                                }
                                case 'q': {
                                        guint16 u;

                                        g_variant_get(s->var, s->sig, &u);
                                        df_fail("-- '%u'\n", u);
                                        FULL_LOG("%u;", u);
                                        break;
                                }
                                case 'i': {
                                        gint32 i;

                                        g_variant_get(s->var, s->sig, &i);
                                        df_fail("-- '%d'\n", i);
                                        FULL_LOG("%d;", i);
                                        break;
                                }
                                case 'h':
                                case 'u': {
                                        guint32 u;

                                        g_variant_get(s->var, s->sig, &u);
                                        df_fail("-- '%u'\n", u);
                                        FULL_LOG("%u;", u);
                                        break;
                                }
                                case 'x': {
                                        gint64 i;

                                        g_variant_get(s->var, s->sig, &i);
                                        df_fail("-- '%" G_GINT64_FORMAT "'\n", i);
                                        FULL_LOG("%" G_GINT64_FORMAT, i);
                                        break;
                                }
                                case 't': {
                                        guint64 u;

                                        g_variant_get(s->var, s->sig, &u);
                                        df_fail("-- '%" G_GUINT64_FORMAT "'\n", u);
                                        FULL_LOG("%" G_GUINT64_FORMAT, u);
                                        break;
                                }
                                case 'd': {
                                        gdouble d;

                                        g_variant_get(s->var, s->sig, &d);
                                        df_fail("-- '%lg'\n", d);
                                        FULL_LOG("%lg;", d);
                                        break;
                                }
                                case 's':
                                case 'o':
                                case 'g': {
                                        _cleanup_(g_freep) gchar *str = NULL;
                                        gchar *ptr;

                                        g_variant_get(s->var, s->sig, &str);
                                        if (str) {
                                                df_fail(" [length: %zu B]-- '%s'\n", strlen(str), str);
                                                ptr = str;
                                                for (; ptr && *ptr; ptr++)
                                                        FULL_LOG("%02x", *ptr & 0xff);
                                        }
                                        FULL_LOG(";");
                                        break;
                                }
                                case 'v': {
                                        _cleanup_(g_freep) gchar *str = NULL;
                                        gchar *ptr;
                                        GVariant *var = NULL;

                                        g_variant_get(s->var, s->sig, var);

                                        if (var && g_variant_check_format_string(var, "s", FALSE)) {
                                                g_variant_get(var, "s", &str);
                                                if (str) {
                                                        df_fail(" [length: %zu B]-- '%s'\n", strlen(str), str);
                                                        ptr = str;
                                                        for (; ptr && *ptr; ptr++)
                                                                FULL_LOG("%02x", *ptr & 0xff);
                                                }
                                                FULL_LOG(";");
                                        } else
                                                df_fail("-- 'unable to deconstruct GVariant instance'\n");
                                        break;
                                }
                                default:
                                        df_fail("Unknown argument signature '%s'\n", s->sig);
                                        return -1;
                        }
                } else {    // advanced argument (array of something, dictionary, ...)
                        df_debug("Not yet implemented in df_fuzz_write_log()\n");
                        return 0;
                }

                s = s->next;
        }

        return 0;
}

/**
 * @function Executes command/script cmd.
 * @param cmd Command/Script to execute
 * @return 0 on successful completition of cmd or when cmd is NULL, value
 * higher than 0 on unsuccessful completition of cmd or -1 on error
 */
static int df_exec_cmd_check(const char *cmd)
{
        if (cmd == NULL)
                return 0;

        const char *fn = "/dev/null";
        _cleanup_(closep) int stdoutcpy = -1, stderrcpy = -1, fd = -1;
        int status = 0;

        fd = open(fn, O_RDWR, S_IRUSR | S_IWUSR);
        if (fd == -1) {
                perror("open");
                return -1;
        }

        // backup std descriptors
        stdoutcpy = dup(1);
        if (stdoutcpy < 0)
                return -1;
        stderrcpy = dup(2);
        if (stderrcpy < 0)
                return -1;

        // make stdout and stderr go to fd
        if (dup2(fd, 1) < 0)
                return -1;
        if (dup2(fd, 2) < 0)
                return -1;
        fd = safe_close(fd);      // fd no longer needed

        // execute cmd
        status = system(cmd);

        // restore std descriptors
        if (dup2(stdoutcpy, 1) < 0)
                return -1;
        stdoutcpy = safe_close(stdoutcpy);
        if (dup2(stderrcpy, 2) < 0)
                return -1;
        stderrcpy = safe_close(stderrcpy);


        if (status == -1)
                return status;
        return WEXITSTATUS(status);
}

static int df_check_if_exited(const int pid) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *line = NULL;
        char proc_pid[14 + DECIMAL_STR_MAX(pid)];
        size_t len = 0;
        int dumping;

        assert(pid > 0);

        sprintf(proc_pid, "/proc/%d/status", pid);

        f = fopen(proc_pid, "r");
        if (!f) {
                if (errno == ENOENT || errno == ENOTDIR)
                        return 0;

                return -1;
        }

        /* Check if the process is not currently dumping a core */
        while (getline(&line, &len, f) > 0) {
                if (sscanf(line, "CoreDumping: %d", &dumping) == 1) {
                        if (dumping > 0)
                                return 0;

                        break;
                }
        }

        /* Assume the process exited if we fail while reading the stat file */
        if (ferror(f))
                return 0;

        return 1;
}

/**
 * @function Function is testing a method in a cycle, each cycle generates
 * data for function arguments, calls method and waits for result.
 * @param statfd FD of process status file
 * @param buf_size Maximum buffer size for generated strings
 * by rand module (in Bytes)
 * @param name D-Bus name
 * @param obj D-Bus object path
 * @param intf D-Bus interface
 * @param pid PID of tested process
 * @param void_method If method has out args 1, 0 otherwise
 * @param execute_cmd Command/Script to execute after each method call.
 * @return 0 on success, -1 on error, 1 on tested process crash, 2 on void
 * function returning non-void value, 3 on warnings and 4 when executed
 * command finished unsuccessfuly
 */
int df_fuzz_test_method(
                long buf_size, const char *name,
                const char *obj, const char *intf, const int pid,
                const int void_method, const char *execute_cmd)
{
        struct df_signature *s = df_list.list;  // pointer on the first signature
        _cleanup_(g_variant_unrefp) GVariant *value = NULL;
        int ret = 0;            // return value from df_fuzz_call_method()
        int execr = 0;          // return value from execution of execute_cmd
        int buf_size_flg = 0;               // if set to 1, buf_size was specified
        // by option -b

        // DEBUG:
        int j = 0;
        df_debug("  Method: %s%s", ansi_bold(), df_list.df_method_name);
        df_debug("(");
        for (; j < df_list.args; j++, s = s->next)
                df_debug(((j < df_list.args - 1) ? "%s, " : "%s"), s->sig);
        df_debug(")%s\n", ansi_normal());


        if (buf_size != 0)
                buf_size_flg = 1;
        if (buf_size < MINLEN)
                buf_size = MAX_BUF_LEN;
        // initialization of random module
        df_rand_init(buf_size);


        df_verbose("  %s...", df_list.df_method_name);

        while (df_rand_continue(df_list.fuzz_on_str_len, df_list.args)) {
                int r;

                value = safe_g_variant_unref(value);

                // creates variant containing all (fuzzed) method arguments
                value = df_fuzz_create_variant();
                if (!value) {
                        if (df_unsupported_sig) {
                                df_unsupported_sig = 0;
                                df_debug("  unsupported argument by dfuzzer: ");
                                df_debug("%s\n", df_unsupported_sig_str);
                                df_unsupported_sig_str = NULL;
                                df_verbose("%s  %sSKIP%s %s - advanced signatures not yet implemented\n",
                                           ansi_cr(), ansi_blue(), ansi_normal(), df_list.df_method_name);
                                return 0;
                        }

                        return df_debug_ret(-1, "Call of df_fuzz_create_variant() returned NULL pointer\n");
                }


                ret = df_fuzz_call_method(value, void_method);
                execr = df_exec_cmd_check(execute_cmd);

                if (execr < 0)
                        return df_fail_ret(-1, "df_exec_cmd_check() failed: %m");
                else if (execr > 0) {
                        df_fail("%s  %sFAIL%s %s - '%s' returned %s%d%s\n",
                                ansi_cr(), ansi_red(), ansi_normal(), df_list.df_method_name,
                                execute_cmd, ansi_red(), execr, ansi_normal());
                        break;
                }

                r = df_check_if_exited(pid);
                if (r < 0)
                        return df_fail_ret(-1, "Error while reading process' stat file: %m");
                else if (r == 0) {
                        ret = -1;
                        df_fail("%s  %sFAIL%s %s - process %d exited\n",
                                ansi_cr(), ansi_red(), ansi_normal(), df_list.df_method_name, pid);
                        break;
                }

                /* Ignore exceptions returned by the test method */
                if (ret == 2)
                        return 0;
                else if (ret > 0)
                        break;

                FULL_LOG("%s;%s;", intf, obj);

                if (logfile)
                        df_fuzz_write_log();
                FULL_LOG("Success\n");
                if (df_except_counter == MAX_EXCEPTIONS) {
                        df_except_counter = 0;
                        break;
                }
        }

        if (ret != 0 || execr != 0)
                goto fail_label;

        df_verbose("%s  %sPASS%s %s\n",
                   ansi_cr(), ansi_green(), ansi_normal(), df_list.df_method_name);
        return 0;


fail_label:
        if (ret != 1) {
                df_fail("   on input:\n");
                FULL_LOG("%s;%s;", intf, obj);
                df_fuzz_write_log();
        }

        df_fail("   reproducer: %sdfuzzer -v -n %s -o %s -i %s -t %s",
                ansi_yellow(), name, obj, intf, df_list.df_method_name);
        if (buf_size_flg)
                df_fail(" -b %ld", buf_size);
        if (execute_cmd != NULL)
                df_fail(" -e '%s'", execute_cmd);
        df_fail("%s\n", ansi_normal());

        if (ret == 1){  // method returning void is returning illegal value
                return 2;
        }
        if (execr > 0){ // command/script execution ended with error
                FULL_LOG("Command execution error\n");
                return 4;
        }
        FULL_LOG("Crash\n");

        return 1;
}

/**
 * @function Creates GVariant tuple variable which contains all the signatures
 * of method arguments including their values. This tuple is constructed
 * from each signature of method argument by one call of g_variant_new()
 * function. This call is constructed dynamically (using libffi) as we don't
 * know number of function parameters on compile time.
 * @return Pointer on a new GVariant variable containing tuple with method
 * arguments
 */
static GVariant *df_fuzz_create_variant(void)
{
        struct df_signature *s = df_list.list;  // pointer on first signature
        // libffi part, to construct dynamic call of g_variant_new() on runtime
        GVariant *val = NULL;
        ffi_cif cif;
        // MAXSIG = max. amount of D-Bus signatures + 1 (format string)
        ffi_type *args[MAXSIG + 1];
        void *values[MAXSIG + 1];
        _cleanup_free_ char *fmt = NULL;
        int ret;

        // creates GVariant for every item signature in linked list
        ret = df_fuzz_create_list_variants();
        if (ret == -1) {
                df_debug("Error in df_fuzz_create_list_variants()\n");
                return NULL;
        } else if (ret == 1) {      // unsupported method signature
                df_unsupported_sig++;
                return NULL;
        }

        fmt = malloc(MAXFMT + 1);
        if (!fmt) {
                df_fail("Error: Could not allocate memory for format string.\n");
                return NULL;
        }
        // creates the format string for g_variant_new() function call
        if (df_fuzz_create_fmt_string(&fmt, MAXFMT + 1) == -1) {
                df_fail("Error: Unable to create format string.\n");
                df_debug("Error in df_fuzz_create_fmt_string()\n");
                return NULL;
        }

        // Initialize the argument info vectors
        args[0] = &ffi_type_pointer;
        values[0] = &fmt;
        for (int i = 1; i <= df_list.args && s; i++) {
                args[i] = &ffi_type_pointer;
                values[i] = &(s->var);
                s = s->next;
        }

        // Initialize the cif
        if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, df_list.args + 1,
                                &ffi_type_pointer, args) == FFI_OK) {
                ffi_call(&cif, FFI_FN(g_variant_new), &val, values);
                // val now holds the result of the call to g_variant_new().
                // When val will be freed, all the floating Gvariants which
                // was used to create it will be freed too, because val is
                // their owner
        } else {
                df_fail("ffi_prep_cif() failed on initializing cif.\n");
                return NULL;
        }

        // GVariant containing method parameters must not be floating, because
        // it would be consumed by g_dbus_proxy_call_sync() function and as
        // result we couldn't have get GVariant values from items of linked list
        // (needed for loging)
        val = g_variant_ref_sink(val);  // converts floating to normal reference
        // so val cannot be consumed
        // by g_dbus_proxy_call_sync() function
        if (g_variant_is_floating(val)) {
                df_fail("Error: Unable to convert GVariant from floating to normal"
                        " reference\n(for method '%s()'.\n", df_list.df_method_name);
                return NULL;
        }

        return val;
}

/**
 * @function Generates data for each method argument according to argument
 * signature and stores it into Gvariant variable in items of linked list.
 * @return 0 on success, 1 on unsupported method signature, -1 on error
 */
static int df_fuzz_create_list_variants(void)
{
        struct df_signature *s = df_list.list;  // pointer on first signature
        int len;

        while (s != NULL) {
                len = strlen(s->sig);
                if (len <= 0) {
                        df_debug("df_fuzz_create_list_variants(): No argument signature\n");
                        return -1;
                } else if (len == 1) {      // one character argument
                        switch (s->sig[0]) {
                                case 'y':
                                        s->var = g_variant_new(s->sig, df_rand_guint8());
                                        break;
                                case 'b':
                                        s->var = g_variant_new(s->sig, df_rand_gboolean());
                                        break;
                                case 'n':
                                        s->var = g_variant_new(s->sig, df_rand_gint16());
                                        break;
                                case 'q':
                                        s->var = g_variant_new(s->sig, df_rand_guint16());
                                        break;
                                case 'i':
                                        s->var = g_variant_new(s->sig, df_rand_gint32());
                                        break;
                                case 'u':
                                        s->var = g_variant_new(s->sig, df_rand_guint32());
                                        break;
                                case 'x':
                                        s->var = g_variant_new(s->sig, df_rand_gint64());
                                        break;
                                case 't':
                                        s->var = g_variant_new(s->sig, df_rand_guint64());
                                        break;
                                case 'd':
                                        s->var =
                                                g_variant_new(s->sig, df_rand_gdouble());
                                        break;
                                case 's': {
                                        _cleanup_(g_freep) gchar *buf = NULL;
                                        if (df_rand_string(&buf) == -1) {
                                                df_debug("In df_rand_string()\n");
                                                return -1;
                                        }
                                        s->var = g_variant_new(s->sig, buf);
                                        break;
                                }
                                case 'o': {
                                        _cleanup_(g_freep) gchar *obj = NULL;
                                        if (df_rand_dbus_objpath_string(&obj) == -1) {
                                                df_debug("In df_rand_dbus_objpath_string()\n");
                                                return -1;
                                        }
                                        s->var = g_variant_new(s->sig, obj);
                                        break;
                                }
                                case 'g': {
                                        _cleanup_(g_freep) gchar *sig = NULL;
                                        if (df_rand_dbus_signature_string(&sig) == -1) {
                                                df_debug("In df_rand_dbus_signature_string()\n");
                                                return -1;
                                        }
                                        s->var = g_variant_new(s->sig, sig);
                                        break;
                                }
                                case 'v': {
                                        GVariant *var;
                                        if (df_rand_GVariant(&var) == -1) {
                                                df_debug("In df_rand_GVariant()\n");
                                                return -1;
                                        }
                                        s->var = g_variant_new(s->sig, var);
                                        break;
                                }
                                case 'h':
                                        s->var = g_variant_new(s->sig, df_rand_unixFD());
                                        break;
                                default:
                                        df_debug("Unknown argument signature '%s'\n", s->sig);
                                        return -1;
                        }
                } else {    // advanced argument (array of something, dictionary, ...)
                        // fprintf(stderr, "Advanced signatures not yet implemented\n");
                        df_unsupported_sig_str = s->sig;
                        for (s = df_list.list; s && s->var; s = s->next) {
                                g_variant_unref(s->var);
                                s->var = NULL;
                        }
                        return 1;   // unsupported method signature
                }

                if (s->var == NULL) {
                        df_fail("Error: Failed to construct GVariant for '%s' signature"
                                "of method '%s'\n", s->sig, df_list.df_method_name);
                        return -1;
                }
                s = s->next;
        }

        return 0;
}

/**
 * @function Creates format string (tuple) from method arguments signatures
 * with maximum length of n-1. The final string is saved in parameter fmt.
 * @param fmt Pointer on buffer where format string should be stored
 * @param n Size of buffer
 * @return 0 on success, -1 on error
 */
static int df_fuzz_create_fmt_string(char **fmt, const int n)
{
        struct df_signature *s = df_list.list;  // pointer on first signature
        int total_len = 0;
        int len = 0;
        char *ptr = *fmt;

        // final fmt string, for example may look like this: "(@s@i)"
        *ptr = '(';
        total_len++;
        ptr++;

        while (s != NULL) {
                len = strlen(s->sig);
                total_len += len + 1;   // including '@' character
                if (total_len > (n - 3)) {
                        df_debug("Format string is too small to consume all signatures\n");
                        return -1;
                }
                *ptr = '@';
                ptr++;
                memcpy(ptr, s->sig, len);
                ptr += len;
                len = 0;
                s = s->next;
        }

        if (total_len > (n - 3)) {
                df_debug("Format string is too small to consume all signatures\n");
                return -1;
        }
        *ptr = ')';
        ptr++;
        *ptr = '\0';

        return 0;
}

/**
 * @function Calls method from df_list (using its name) with its arguments.
 * @param value GVariant tuple containing all method arguments signatures and
 * their values
 * @param void_method If method has out args 1, 0 otherwise
 * @return 0 on success, -1 on error, 1 if void method returned non-void
 * value or 2 when tested method raised exception (so it should be skipped)
 */
static int df_fuzz_call_method(GVariant *value, const int void_method)
{
        _cleanup_(g_error_freep) GError *error = NULL;
        _cleanup_(g_variant_unrefp) GVariant *response = NULL;
        _cleanup_(g_freep) gchar *dbus_error = NULL;
        const gchar *fmt;

        // Synchronously invokes method with arguments stored in value (GVariant *)
        // on df_dproxy.
        response = g_dbus_proxy_call_sync(
                        df_dproxy,
                        df_list.df_method_name,
                        value,
                        G_DBUS_CALL_FLAGS_NONE,
                        -1,
                        NULL,
                        &error);
        if (!response) {
                // D-Bus exceptions are accepted
                dbus_error = g_dbus_error_get_remote_error(error);
                if (dbus_error) {
                        // if process does not respond
                        if (strcmp(dbus_error, "org.freedesktop.DBus.Error.NoReply") == 0)
                                return -1;
                        else if (strcmp(dbus_error, "org.freedesktop.DBus.Error.Timeout") == 0) {
                                sleep(10);      // wait for tested process; processing
                                // of longer inputs may take a longer time
                                return -1;
                        } else if ((strcmp(dbus_error, "org.freedesktop.DBus.Error.AccessDenied") == 0) ||
                                   (strcmp(dbus_error, "org.freedesktop.DBus.Error.AuthFailed") == 0)) {
                                df_verbose("%s  %sSKIP%s %s - raised exception '%s'\n",
                                           ansi_cr(), ansi_blue(), ansi_normal(),
                                           df_list.df_method_name, dbus_error);
                                return 2;
                        }
                }

                g_dbus_error_strip_remote_error(error);
                if (strstr(error->message, "Timeout")) {
                        df_verbose("%s  %sSKIP%s %s - timeout reached\n",
                                   ansi_cr(), ansi_blue(), ansi_normal(), df_list.df_method_name);
                        return 2;
                }

                df_debug("%s  EXCE %s - D-Bus exception thrown: %.60s\n",
                         ansi_cr(), df_list.df_method_name, error->message);
                df_except_counter++;
                return 0;
        } else {
                if (void_method) {
                        // fmt points to GVariant, do not free it
                        fmt = g_variant_get_type_string(response);
                        // void function can only return empty tuple
                        if (strcmp(fmt, "()") != 0) {
                                df_fail("%s  %sFAIL%s %s - void method returns '%s' instead of '()'\n",
                                        ansi_cr(), ansi_red(), ansi_normal(), df_list.df_method_name, fmt);
                                return 1;
                        }
                }
        }

        return 0;
}

/**
 * @function Releases memory used by this module. This function must be called
 * after df_fuzz_add_method() and df_fuzz_add_method_arg() functions calls
 * after the end of fuzz testing of each method.
 */
void df_fuzz_clean_method(void)
{
        free(df_list.df_method_name);

        // frees the linked list
        struct df_signature *tmp;
        while (df_list.list != NULL) {
                tmp = df_list.list->next;
                free(df_list.list->sig);
                free(df_list.list);
                df_list.list = tmp;
        }
}
