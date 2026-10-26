{
  description = "wwal (WayWal) - Zero-copy Wayland wallpaper, VA-API video, Shadertoy, and Material You engine in C23";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
      pkgsFor = system: import nixpkgs { inherit system; };
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = pkgsFor system;
        in
        rec {
          wwal = pkgs.stdenv.mkDerivation {
            pname = "wwal";
            version = "1.0.0";
            src = self;

            nativeBuildInputs = with pkgs; [
              meson
              ninja
              pkg-config
              python3
              wayland-scanner
              scdoc
              makeWrapper
            ];

            buildInputs = with pkgs; [
              wayland
              wayland-protocols
              libdrm
              mesa
              libva
              liburing
              libseccomp
              libpng
              libjpeg_turbo
              libwebp
              libavif
              libjxl
              librsvg
              pipewire
            ];

            mesonFlags = [
              "-Denable_vaapi=true"
              "-Denable_io_uring=true"
              "-Denable_simd=true"
              "-Denable_seccomp=true"
              "-Denable_systemd=true"
            ];

            meta = with pkgs.lib; {
              description = "Zero-copy C23 Wayland wallpaper, hardware video, GLSL shader, and Material You engine";
              homepage = "https://github.com/Execorn/wwal";
              license = licenses.gpl3Plus;
              platforms = platforms.linux;
              mainProgram = "wwal";
            };
          };

          default = wwal;
        });

      devShells = forAllSystems (system:
        let
          pkgs = pkgsFor system;
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.default ];
            packages = with pkgs; [
              clang-tools
              gdb
              valgrind
              ffmpeg
              shaderc
              weston
            ];
          };
        });

      homeManagerModules.default = { config, lib, pkgs, ... }:
        let
          cfg = config.services.wwal;
          tomlFormat = pkgs.formats.toml { };
        in
        {
          options.services.wwal = {
            enable = lib.mkEnableOption "wwal Wayland wallpaper daemon and client";

            package = lib.mkOption {
              type = lib.types.package;
              default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
              description = "The wwal package to install and run.";
            };

            systemdSocketActivation = lib.mkOption {
              type = lib.types.bool;
              default = true;
              description = "Enable display-aware systemd user socket activation for wwald.";
            };

            swwwCompatibility = lib.mkOption {
              type = lib.types.bool;
              default = false;
              description = "Install drop-in swww and awww CLI compatibility symlinks.";
            };

            settings = lib.mkOption {
              type = tomlFormat.type;
              default = { };
              example = lib.literalExpression ''
                {
                  daemon = {
                    layer = "background";
                    battery_policy = "reduce-fps";
                    restore_on_start = true;
                  };
                  theme = {
                    enable = true;
                    caelestia_sync = true;
                    terminal_osc = true;
                  };
                  transition = {
                    type = "fade";
                    duration = 0.8;
                    fps = 144;
                    bezier = [ 0.25 0.1 0.25 1.0 ];
                  };
                }
              '';
              description = "Declarative configuration written to ~/.config/wwal/config.toml.";
            };
          };

          config = lib.mkIf cfg.enable {
            home.packages = [ cfg.package ]
              ++ lib.optional cfg.swwwCompatibility (pkgs.runCommand "wwal-swww-compat" { } ''
                mkdir -p $out/bin
                ln -s ${cfg.package}/bin/wwal $out/bin/swww
                ln -s ${cfg.package}/bin/wwald $out/bin/swww-daemon
                ln -s ${cfg.package}/bin/wwal $out/bin/awww
                ln -s ${cfg.package}/bin/wwald $out/bin/awww-daemon
              '');

            xdg.configFile."wwal/config.toml" = lib.mkIf (cfg.settings != { }) {
              source = tomlFormat.generate "wwal-config.toml" cfg.settings;
            };

            systemd.user.sockets.wwald = lib.mkIf cfg.systemdSocketActivation {
              Unit = {
                Description = "WayWal (Wayland Wallpapers) Daemon IPC Socket";
                Documentation = "https://github.com/Execorn/wwal";
                PartOf = [ "graphical-session.target" ];
              };
              Socket = {
                ListenStream = "%t/wwald.sock";
                SocketMode = "0600";
                RemoveOnStop = true;
              };
              Install = {
                WantedBy = [ "sockets.target" "graphical-session.target" ];
              };
            };

            systemd.user.services.wwald = {
              Unit = {
                Description = "WayWal (Wayland Wallpapers) Daemon";
                Documentation = "https://github.com/Execorn/wwal";
                PartOf = [ "graphical-session.target" ];
                After = [ "graphical-session.target" ];
                ConditionEnvironment = "WAYLAND_DISPLAY";
              };
              Service = {
                Type = "simple";
                ExecStart = "${cfg.package}/bin/wwald";
                Sockets = lib.mkIf cfg.systemdSocketActivation [ "wwald.socket" ];
                Restart = "on-failure";
                RestartSec = "1s";
              };
              Install = {
                WantedBy = [ "graphical-session.target" ];
              };
            };
          };
        };
    };
}
