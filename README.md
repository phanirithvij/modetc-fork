# modetc

### Move your dotfiles from kernel space

modetc is a Linux kernel module that rewrites paths in file operations: it
allows you to move files wherever you like, while still having programs finding
them where they expected them to be.

The main application is to move the dotfiles of those stubborn programs that
refuse to adopt the [XDG basedir] standard, away from the home directory. For
example, you can rewrite the path `~/.ssh` to `~/var/lib/ssh`.
Yes, this is the nuclear option.

[XDG basedir]: https://specifications.freedesktop.org/basedir-spec/latest/


## Configuration

modetc is configured via module parameters that are set either when manually
loading the module (eg. `modprobe modetc homedir=...`) or by adding a line to
`/etc/modprobe.conf`. The parameters are:

| Name           | Default value    | Description                              |
|:---------------|:----------------:|:-----------------------------------------|
| `homedir`      |       N/A        | Home directory where to rewrite paths    |
| `default_rule` |       N/A        | The default replacement for all dotfiles |
| `rules_file`   | /etc/modetc.conf | File containing the rewriting rules      |
| `debug`        |       0          | Turn on debugging for rewriting rules    |


## Rewriting rules

modetc rules are pure text search and replace: single substitution per file
path, no wildcards nor regular expresssions.

The rules are only applied to absolute path with the `homedir` prefix followed
by `.`, or to any relative path with a leading `.` when the working directory
is `homedir`.
Rules are tested in the order in which they appear in the rules file and the
first to match will be applied. If no specific rule matches, the leading `.` of
the file path will be replaced with the value of `default_rule`.

Note: if `default_rule` is unset or the empty string, dotfiles that don't
       match any rule will not be rewritten.

The rules file consists of one or more lines with one rule per line of the
following format
```
<match>	<replacement>
```

Note: whitespace is significant and match and replacement are separate by a tab
character. On a given line, anything after a `#` character is treated as a
comment. Empty lines are also ignored.

Note: the maximum number of rules supported is 16.

### Example rules file:

```
# Garbage found in XDG dirs
var/lib/recently-used.xbel	var/cache/recently-used
etc/Trolltech.conf	var/cache/qt/cache
etc/QtProject.conf	var/cache/qt/conf
etc/matplotlib/	var/cache/matplotlib/

# Generic rewrites
.compose-cache	var/cache/compose
.nix-	var/lib/nix/

# X.org files
.Xresources	etc/x/resources
.X	var/cache/x/
.x	var/cache/x/

# XDG "compliant" programs
.config/	etc/
.local/state/	var/lib/
.cache/	var/cache/
```

## Runtime commands

You can send modetc some commands like reloading the rules file or pausing the
rewriting by writing them to `/proc/modetc`. For example
```
echo pause | sudo tee /proc/modetc
```
Use `cat /proc/modetc` to get a list of commands and what they do.


## Building

If using [Nix], to build the module simply run
```
nix-build -A module
```
in the project directory. Before actually loading the module, it is a good idea
to test that it works correctly in a virtual machine, so run:
```
nix-build -A test
```
If the test passed, you can safely load the module with
```
sudo insmod result/lib/modules/*/misc/modetc.ko
```

Note: when using a custom or older kernel, you may have to change the
`linux` argument in the Nix expression (`default.nix`) to match yours.

Otherwise, install a basic toolchain and linux kernel module and run
`make` to build the module.


[Nix]: https://nixos.org/download/#download-nix


## Installing

On NixOS, add the following to your configuration:
```nix
{ config, pkgs, lib, ...  }:

let
  modetc = import (pkgs.fetchzip {
    url = "https://maxwell.eurofusion.eu/git/rnhmjoj/modetc/archive/v0.1.4.zip";
    sha256 = lib.fakeHash;  # change this
  }) { inherit pkgs; linux = config.boot.kernelPackages; };
in
{
  boot.kernelModules = [ "modetc" ];
  boot.extraModulePackages = [ modetc.module ];
  boot.extraModprobeConfig = ''
    options modetc homedir=/home/alice default_rule=var/lib/
  '';

  environment.etc."modetc.conf".text = ''
    # add your rules here
  '';
}
```
This installs modetc, loads it on boot and configures it.

On other distributions, after building the module run
```
# make install
# depmod -a
```
The module can now be loaded with `sudo modprobe modetc homedir=...`.


## How it works

[kprobes] are a mechanism to insert breakpoints at arbitrary locations in the
Linux kernel code; they are a performance or debugging tool, but can also be
used to alter the kernel behaviour at runtime.

modetc inserts kprobes in a few functions of the Virtual File System (VFS)
layer and system calls implementations (`do_symlinkat`, `do_rmdir`, etc.) that
handle file paths. When any of these functions is called, a breakpoint is hit
and a small piece of code is inserted that modifies in-place the file path
argument, so the file operation is carried out on the replacement file.

Note that only the cache containing the kernel space copy of a userspace file
path (`names_cachep`) is actually changed, so the process that invoked the
system call is completely oblivious to any of this.

[kprobes]: https://docs.kernel.org/trace/kprobes.html


## Comparison with alternatives

[libetc] was an earlier solution based on intercepting the libc wrapper functions
of the system calls using the `LD_PRELOAD` mechanism. Unlike libetc, modetc

- works for statically linked programs
- works for programs not using libc to invoke the system calls (e.g. sqlite)
- can be quickly toggled without restarting programs

[rewritefs] solves most of the issues of libetc by rewriting the paths at the
filesystem level, using a special FUSE filesystem. However, it *severely*
degrades the performance of any I/O operation under the home directory.
In comparison, modetc

- does not require to mount a file system
- runs in the kernel, with very little overhead
- does not support regular expressions
- can be quickly toggled without logging out

Similarly to both method, modetc is Linux-specific.

[libetc]: https://ordiluc.net/fs/libetc/README
[rewritefs]: https://github.com/sloonz/rewritefs
[FUSE]: https://en.wikipedia.org/wiki/Filesystem_in_Userspace


## License

Copyright (C) 2025 Michele Guerini Rocco

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.
