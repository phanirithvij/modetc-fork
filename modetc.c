/* Copyright (C) 2025 Michele Guerini Rocco
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

/* prefix for log messages */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define is_fname_end(c) (c == '\0' || c == '/')

/* for kernel module programming */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>

/* for module parameters */
#include <linux/moduleparam.h>

/* for procfs interface*/
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/file.h>

/* for function interception */
#include <linux/kprobes.h>

/* for filepath lookup */
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/path.h>


MODULE_DESCRIPTION("Rewrite paths to enforce the XDG basedir spec");
MODULE_AUTHOR("Michele Guerini Rocco <rnhmjoj@inventati.org>");
MODULE_VERSION("0.1");
MODULE_LICENSE("GPL");


/* Module parameters */

static char  *homedir;
static size_t homedir_len;
module_param(homedir, charp, 0660);
MODULE_PARM_DESC(homedir, "Home directory where to rewrite paths");

static char  *default_rule;
module_param(default_rule, charp, 0660);
MODULE_PARM_DESC(default_rule, "The default replacement for all dotfiles");

static char *rules_file = "/etc/modetc.conf";
module_param(rules_file, charp, 0660);
MODULE_PARM_DESC(rules_file, "File containing the rewriting rules, default: /etc/modetc.conf");

static int debug = 0;
module_param(debug, int, 0660);
MODULE_PARM_DESC(debug, "Turn on debugging for rewriting rules, default 0");


/* Rules file */

#define MAX_RULES 16
#define MATCH_SIZE 32
#define MAX_RULES_SIZE MAX_RULES*(MATCH_SIZE + 1)*2

struct rule {
  char match[32]; // substring to match
  size_t len;     // length of match
  char repl[32];  // replacement
};

static struct rule rules[MAX_RULES];
static int nrules = 0;


static int load_rules(void) {
  char buffer[MAX_RULES_SIZE];
  loff_t pos = 0;

  // Open the file
  struct file *file = filp_open(rules_file, O_RDONLY, 0);
  if (IS_ERR(file)) {
    pr_err("failed to rules file\n");
    return -EIO;
  }

  // Read all file into buffer
  int count = kernel_read(file, buffer, sizeof(buffer), &pos);
  if (count < 0) {
    pr_err("failed to read rules file\n");
    filp_close(file, NULL);
    return count;
  }
  filp_close(file, NULL);

  // Parse the rules file
  nrules = 0;
  int comment = 0;
  loff_t start = 0;
  for (int pos = 0; pos < count; pos++) {
    // Start of comment
    if (!comment) {
      comment = buffer[pos] == '#' && (pos > 0? buffer[pos-1] == '\n' : 1);
    }
    // End of match string
    if (!comment && buffer[pos] == '\t') {
      size_t len = pos - start;
      snprintf(rules[nrules].match, min(MATCH_SIZE, len+1), "%s", buffer+start);
      rules[nrules].len = len;
      start = pos + 1;
    }
    // End of replacement string
    if (buffer[pos] == '\n' && pos != start) {
      int len = pos - start;
      snprintf(rules[nrules].repl, min(MATCH_SIZE, len+1), "%s", buffer+start);
      start = pos + 1;
      if (!comment) nrules++;
      comment = 0;
    }
    if (nrules > MAX_RULES_SIZE) break;
  }

  // Print parsed rules
  pr_info("%d rules loaded\n", nrules);
  if (debug) {
    for (int i = 0; i < nrules; i++)
      pr_info("rule %d: '%s' -> '%s'\n", i, rules[i].match, rules[i].repl);
  }

  return 0;
}


/* procfs interface */

static int paused = 0;

static struct proc_dir_entry *modetc_proc_entry;

static ssize_t modetc_proc_write(
  struct file *file,
  const char __user *ubuf,
  size_t count,
  loff_t *ppos)
{
  char cmd[256];

  // Read command
  if (copy_from_user(cmd, ubuf, sizeof(cmd))) return -EFAULT;
  if (debug) pr_info("command \"%s\"\n", cmd);

  // Parse command
  if (!strncmp(cmd, "reload", 6)) {
    pr_info("reloading\n");
    if (load_rules() < 0) pr_warn("running with no rules\n");
  }
  else if (!strncmp(cmd, "pause", 5)) {
    pr_info("rewriting paused\n");
    paused = 1;
  }
  else if (!strncmp(cmd, "resume", 6)) {
    pr_info("rewriting resumed\n");
    paused = 0;
  }
  else if (!strncmp(cmd, "debug", 5)) {
    debug = !debug;
    if (debug) pr_info("debugging enabled\n");
    if (!debug) pr_info("debugging disabled\n");
  }
  else {
    pr_warn("invalid command '%s'\n", cmd);
    return -EINVAL;
  }

  return count;
}

static ssize_t modetc_proc_read(
  struct file *file,
  char __user *ubuf,
  size_t count,
  loff_t *ppos)
{
  // Return help
  const char help[] =
    "# modetc commands\n\n"
    "You can write one of the following commands to this file:\n"
    "1. reload:\tMakes modetc reload the rules file.\n"
    "2. pause:\tStop rewriting paths.\n"
    "3. resume:\tResume rewriting paths.\n"
    "4. debug:\tToggle debug messages.\n";

  return simple_read_from_buffer(ubuf, count, ppos, help, sizeof(help));
}

static struct proc_ops modetc_proc_ops =
  { .proc_read  = modetc_proc_read,
    .proc_write = modetc_proc_write,
  };


/* Filepath rewriting */

/* Gets the working directory path of the current process
 *
 * Notes:
 * 1. `current` holds the current process info, which includes the
 *    filesystem struct.
 * 2. `get_fs_pwd` returns a path struct (not a string), but there's
 *    `d_path` that does the convertion.
 * 3. `d_path` doesn't write at the start of buffer but returns a
 *    pointer to the start or an error.
 */
static char *get_cwd_path(char *buf, size_t len) {
  struct path cwd;
  get_fs_pwd(current->fs, &cwd);
  return d_path(&cwd, buf, len);
}


/*
 * Overwrites a filepath in buffer of fixed size EMBEDDED_NAME_MAX
 * following the given rewriting rules
 */
static int do_rewrite(const char *caller, int dfd, struct filename *fname) {
  // Skip if any error occurred
  if (IS_ERR(fname)) {
    pr_err("getname failed, err=%ld\n", PTR_ERR(fname));
    return 0;
  }

  // Exit immediately when paused
  if (unlikely(paused)) return 0;

  #define EMBEDDED_NAME_MAX (PATH_MAX - offsetof(struct filename, iname))
  char fname1[EMBEDDED_NAME_MAX];
  const char *cursor = fname->name;

  // Handle relative paths
  if (unlikely(cursor[0] != '/')) {
    // TODO: handle these, for now skip
    if (dfd != AT_FDCWD) return 0;

    // Get the cwd
    char buf[EMBEDDED_NAME_MAX];

    char *cwd = get_cwd_path(buf, sizeof(buf));
    if (IS_ERR(cwd)) {
      pr_err("failed to get cwd, err=%ld\n", PTR_ERR(cwd));
      pr_err("could not rewrite %s\n", fname->name);
      return 0;
    }

    // cwd is not home, skip
    if (likely(strcmp(cwd, homedir))) return 0;
  } else {
    // Handle absolute paths

    // Outside home, skip
    if (strncmp(cursor, homedir, homedir_len)) return 0;

    // Move past the home prefix
    cursor += homedir_len + 1;
  }

  // Skip non-dotfiles
  if (likely(cursor[0] != '.')) return 0;

  // Skip special . and .. directories
  if (unlikely(is_fname_end(cursor[1]))) return 0;
  if (unlikely(cursor[1] == '.' && is_fname_end(cursor[2]))) return 0;

  if (debug) pr_info("[%s] intercepted path %s\n", caller, fname->name);

  // Replace with rules
  size_t len = 0;
  int i;
  for (i = 0; i < nrules; i++) {
    const struct rule *r = &rules[i];

    if (!strncmp(cursor, r->match, r->len)) {
      cursor += r->len;
      len = snprintf((char *)fname1, sizeof(fname1), "%s/%s%s",
                     homedir, r->repl, cursor);
      if (debug) pr_info("[%s] path %s matches rule %d\n",
                         caller, fname->name, i);
      break;
    }
  }

  // Default rule
  if (i == nrules) {
    // Strip leading dot
    cursor += 1;
    len = snprintf((char *)fname1, sizeof(fname1), "%s/%s%s",
                          homedir, default_rule, cursor);
    if (debug) pr_info("[%s] default rule for %s\n", caller, fname->name);
  }

  // If anything matched and result fits the buffer
  if (len > 0 && len < EMBEDDED_NAME_MAX) {
    // Overwrite original filename
    if (debug) pr_info("[%s] rewriting %s -> %s\n",
                       caller, fname->name, fname1);
    memcpy((void *)fname->name, fname1, len + 1);
  }

  return 0;
}


/* Handlers that modify in-place one or more arguments
 * of a kernel function with type `struct filename`
 */

static int handle_filename0(struct kprobe *kp, struct pt_regs *regs)
{
  int dfd = (int)regs_get_kernel_argument(regs, 1);
  struct filename *f = (struct filename *)regs_get_kernel_argument(regs, 0);
  return do_rewrite(kp->symbol_name, dfd, f);
}

static int handle_filename1(struct kprobe *kp, struct pt_regs *regs)
{
  int dfd = (int)regs_get_kernel_argument(regs, 0);
  struct filename *f = (struct filename *)regs_get_kernel_argument(regs, 1);
  return do_rewrite(kp->symbol_name, dfd, f);
}

static int handle_filename2(struct kprobe *kp, struct pt_regs *regs)
{
  int dfda = (int)regs_get_kernel_argument(regs, 0);
  struct filename *fa = (struct filename *)regs_get_kernel_argument(regs, 1);
  do_rewrite(kp->symbol_name, dfda, fa);

  int dfdb = (int)regs_get_kernel_argument(regs, 2);
  struct filename *fb = (struct filename *)regs_get_kernel_argument(regs, 3);
  do_rewrite(kp->symbol_name, dfdb, fb);

  return 0;
}


/* All kprobes used to intercept and rewrite filenames */
static struct kprobe probes[] =
{
  { .symbol_name="do_unlinkat",     .pre_handler=handle_filename1 },
  { .symbol_name="do_symlinkat",    .pre_handler=handle_filename0 },
  { .symbol_name="do_rmdir",        .pre_handler=handle_filename1 },
  { .symbol_name="do_renameat2",    .pre_handler=handle_filename2 },
  { .symbol_name="do_filp_open",    .pre_handler=handle_filename1 },
  { .symbol_name="vfs_statx",       .pre_handler=handle_filename1 },
  { .symbol_name="filename_create", .pre_handler=handle_filename1 },
  { .symbol_name="filename_lookup", .pre_handler=handle_filename1 },
};

/* Registers and kprobe while logging any error */
static int add_probe(struct kprobe *kp)
{
  int ret = register_kprobe(kp);
  if (ret < 0) {
    pr_err("failed to register %s kprobe\n", kp->symbol_name);
    pr_err("error %d\n", ret);
  }
  else {
    pr_info("%s kprobe registered\n", kp->symbol_name);
  }

  return ret;
}


/* Kernel module hooks */

static int __init modetc_init(void)
{
  if (debug)
    pr_info("params: homedir=%s,default_rule=%s,debug=%d,rules_file=%s\n",
            homedir, default_rule, debug, rules_file);

  homedir_len = strlen(homedir);
  modetc_proc_entry = proc_create("modetc", 0660, NULL, &modetc_proc_ops);

  for (int i = 0; i < ARRAY_SIZE(probes); i++) {
    add_probe(&probes[i]);
  }

  if (load_rules() < 0) pr_warn("running with no rules\n");

  pr_info("started\n");
  return 0;
}

static void __exit modetc_cleanup(void)
{
  proc_remove(modetc_proc_entry);
  for (int i = 0; i < ARRAY_SIZE(probes); i++) {
    unregister_kprobe(&probes[i]);
  }
  pr_info("stopped\n");
}

module_init(modetc_init);
module_exit(modetc_cleanup);
