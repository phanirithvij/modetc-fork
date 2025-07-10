{ pkgs ? import <nixpkgs> { }
, linux ? pkgs.linuxPackages_latest
}:

rec {

module = pkgs.stdenv.mkDerivation {
  name = "modetc";
  version = builtins.readFile ./.version;
  src = builtins.filterSource (path: type:
    baseNameOf path == "modetc.c" ||
    baseNameOf path == "Makefile"
  )./.;

  hardeningDisable = [ "pic" "format" ];
  nativeBuildInputs = linux.kernel.moduleBuildDependencies;

  makeFlags = [
    "KERNELRELEASE=${linux.kernel.modDirVersion}"
    "KERNEL_DIR=${linux.kernel.dev}/lib/modules/${linux.kernel.modDirVersion}/build"
    "INSTALL_MOD_PATH=$(out)/lib/modules/${linux.kernel.modDirVersion}/misc/"
  ];

  meta = with pkgs.lib; {
    homepage = "https://maxwell.eurofusion.eu/git/rnhmjoj/modetc";
    description = "Move your dotfiles from kernel space";
    maintainers = [ maintainers.rnhmjoj ];
    platforms = platforms.linux;
    license = licenses.gpl3Plus;
  };
};

test = pkgs.nixosTest {
  name = "modetc";
  nodes.machine = { lib, modulesPath, ... }: {
    imports = [
      "${modulesPath}/profiles/minimal.nix"
      "${modulesPath}/profiles/headless.nix"
    ];

    # Disable useless stuff
    networking.useDHCP = false;
    systemd.services.systemd-logind.enable = false;
    systemd.oomd.enable = false;
    services.dbus.enable = lib.mkForce false;
    services.nscd.enable = false;
    system.nssModules = lib.mkForce [ ];

    boot.kernelPackages = linux;
    boot.kernelModules = [ "modetc" ];
    boot.extraModulePackages = [ module ];
    boot.extraModprobeConfig = ''
      options modetc homedir=/home/alice default_rule=var/lib/ debug=1
    '';

    users.users.alice = {
      isNormalUser = true;
    };

    environment.etc."modetc.conf".text = ''
      # Garbage found in XDG dirs
      var/lib/recently-used.xbel	var/cache/recently-used
      etc/Trolltech.conf	var/cache/qt/cache
      etc/QtProject.conf	var/cache/qt/conf
      var/cache/qt/	var/cache/qt/
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
    '';
  };
  testScript = ''
    import re


    def modetc_logs(msg):
        """
        Check that modetc emits `msg` to the kernel ring buffer
        """
        machine.wait_for_console_text("modetc: " + re.escape(msg))


    machine.start()
    with subtest("Module is loaded successfully"):
        modetc_logs("started")

    with subtest("Procfs interface is working"):
        machine.succeed("grep '# modetc commands' /proc/modetc >&2")

    with subtest("Module reloading works"):
        machine.execute("echo reload > /proc/modetc")
        modetc_logs("reloading")
        modetc_logs("13 rules loaded")

    with subtest("Filename interception is working"):
        # relative
        machine.execute("cd /home/alice; cat .dotfile")
        modetc_logs("[do_filp_open] intercepted path .dotfile")

        # absolute
        machine.execute("stat /home/alice/.another")
        modetc_logs("[vfs_statx] intercepted path /home/alice/.another")

        # unrelated
        machine.execute("ls /.yetanother")
        machine.fail("dmesg | grep -qF '/.yetanother'")

        # relative under, but not home
        machine.succeed("mkdir -p /home/alice/repo/.git")
        machine.succeed("cd /home/alice/repo/; touch .git/index")
        machine.fail("dmesg | grep -qF '.git'")
        machine.succeed("ls -a /home/alice/repo/ | grep .git")

        # absolute under, but not home
        machine.succeed("touch /home/alice/repo/.gitignore")
        machine.fail("dmesg | grep -qF '.gitignore'")

    with subtest("Filename rewriting is working"):
        # create dirs
        machine.succeed("mkdir -p /home/alice/etc/x")
        machine.succeed("mkdir -p /home/alice/var/{cache/x,lib}")

        # specific rule
        machine.execute("echo test1 > /home/alice/.Xresources")
        modetc_logs(
          "[do_filp_open] rewriting /home/alice/.Xresources "
          "-> /home/alice/etc/x/resources"
        )
        machine.succeed("grep test1 /home/alice/etc/x/resources")

        # another rule
        machine.execute("echo test2 > /home/alice/.Xauthority")
        modetc_logs(
          "[do_filp_open] rewriting /home/alice/.Xauthority -> "
          "/home/alice/var/cache/x/authority"
        )
        machine.succeed("grep test2 /home/alice/var/cache/x/authority")

        # default rule
        machine.succeed("cd /home/alice; touch .history")
        modetc_logs(
          "[do_filp_open] rewriting .history -> "
          "/home/alice/var/lib/history"
        )
        machine.succeed("ls /home/alice/var/lib/history")

    with subtest("Can pause and resume rewriting"):
        machine.succeed("echo pause > /proc/modetc")
        modetc_logs("rewriting paused")
        machine.fail("stat /home/alice/.history")

        machine.succeed("echo resume > /proc/modetc")
        modetc_logs("rewriting resumed")
        machine.succeed("stat /home/alice/.history")

    with subtest("Can toggle debugging"):
        # debugging off
        machine.succeed("echo debug > /proc/modetc")
        modetc_logs("debugging disabled")
        machine.execute("stat /home/alice/.probe")
        machine.fail("dmesg | grep -q probe")

        # debugging on
        machine.succeed("echo debug > /proc/modetc")
        modetc_logs("debugging enabled")
        machine.execute("stat /home/alice/.probe")
        modetc_logs("[vfs_statx] intercepted path /home/alice/.probe")


    with subtest("Check more syscalls are working"):
        # stat
        machine.succeed("touch /home/alice/var/lib/bla")
        machine.succeed("stat /home/alice/.bla")

        # chmod
        machine.succeed("chmod +x /home/alice/.bla")

        # chown
        machine.succeed("chown alice:users /home/alice/.bla")

        # mkdir
        machine.succeed("mkdir /home/alice/.bop")
        machine.fail("ls /home/alice | grep -q bop")
        machine.succeed("stat /home/alice/var/lib/bop")

        # chdir
        machine.succeed("cd /home/alice/.bop")

        # rmdir
        machine.succeed("rmdir /home/alice/.bop")
        machine.fail("stat /home/alice/var/lib/bop")

        # mknod
        machine.succeed("mknod /home/alice/.null c 1 3")
        machine.fail("ls /home/alice | grep -q null")

        # truncate
        machine.succeed("truncate -s 1K /home/alice/.empty")
        machine.succeed("stat /home/alice/var/lib/empty")

        # unlink
        machine.succeed("rm /home/alice/.null")

        # rename
        machine.succeed("mv /home/alice/.{bla,blu}")
        machine.fail("ls /home/alice | grep -q 'bla|blu'")
        machine.succeed("stat /home/alice/var/lib/blu")

        # link
        machine.succeed("touch /home/alice/var/lib/dest")
        machine.succeed("ln /home/alice/.{dest,hard}")
        machine.succeed("stat /home/alice/var/lib/hard")

        # symlink
        machine.succeed("ln -s /home/alice/.{dest,alias}")
        machine.succeed("stat /home/alice/var/lib/alias")

        # readlink
        res = machine.succeed("readlink /home/alice/.alias")
        assert res.startswith("/home/alice/var/lib/dest")

        # bind/connect
        machine.execute("nc -lU '/home/alice/.socket' >&2 &")
        machine.wait_until_succeeds("stat /home/alice/var/lib/socket")
        machine.succeed("echo ciao | nc -NU /home/alice/.socket")

    with subtest("Can unload the module"):
        machine.succeed("rmmod modetc")
        modetc_logs("stopped")
  '';
};

}
