{
  description = "Couch-console input tools. evdev-interposer: virtual keyboard/pointer devices over grabbed evdev devices, input gated on the TV's CEC state, CEC logical-address claiming, and a pop-one-level command from a key or pad chord. evdev-bridge: holds an interposer bridge session that switches the TV to another input and forwards local input to it over CEC. evdev-chords: passive chord observer firing configured commands, sharing the interposer's chord/device libraries.";

  inputs = {
    flake-lib.url = "github:jgus-org/flake-lib/v1";
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs =
    {
      self,
      flake-lib,
      nixpkgs,
    }:
    flake-lib.lib.eachDefaultLinuxSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };

        orgFormatting = flake-lib.lib.mkFormatting {
          inherit self system;
        };

        evdev-tools = pkgs.stdenv.mkDerivation {
          buildInputs = [
            pkgs.systemd
            pkgs.boost
          ];
          buildPhase = ''
            runHook preBuild
            ''${CXX} -O3 -Wall -Wextra -pipe -std=c++23 -o evdev-interposer main-evdev-interposer.cpp daemon.cpp devices.cpp chord.cpp gate.cpp \
              $(pkg-config --cflags --libs libsystemd) -lboost_program_options -lboost_json
            ''${CXX} -O3 -Wall -Wextra -pipe -std=c++23 -o evdev-chords main-evdev-chords.cpp devices.cpp chord.cpp \
              -lboost_json
            ''${CXX} -O3 -Wall -Wextra -pipe -std=c++23 -o evdev-bridge main-evdev-bridge.cpp \
              -lboost_program_options
            ''${CXX} -O2 -Wall -Wextra -std=c++23 -I. -o interposer_test \
              test/interposer_test.cpp devices.cpp chord.cpp gate.cpp \
              $(pkg-config --cflags --libs gtest gtest_main) -lboost_json
            runHook postBuild
          '';
          checkInputs = [ pkgs.gtest ];
          checkPhase = ''
            runHook preCheck
            ./interposer_test
            runHook postCheck
          '';
          doCheck = true;
          installPhase = ''
            runHook preInstall
            install -Dm755 evdev-interposer "$out/bin/evdev-interposer"
            install -Dm755 evdev-chords "$out/bin/evdev-chords"
            install -Dm755 evdev-bridge "$out/bin/evdev-bridge"
            runHook postInstall
          '';
          meta = {
            description = "Couch-console input tools: the CEC-gated keyboard/pointer interposer, the evdev-bridge session holder, and the passive evdev-chords observer";
            mainProgram = "evdev-interposer";
            platforms = flake-lib.lib.defaultLinuxSystems;
          };
          nativeBuildInputs = [ pkgs.pkg-config ];
          pname = "evdev-tools";
          src = ./.;
          version = "0.1";
        };
      in
      {
        checks.formatting = orgFormatting.check;
        formatter = orgFormatting.formatter;
        packages = {
          inherit evdev-tools;
          default = evdev-tools;
        };
      }
    );
}
