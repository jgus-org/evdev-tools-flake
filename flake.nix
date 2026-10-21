{
  description = "Couch-console input tools. evdev-interposer: virtual keyboard/pointer devices over grabbed evdev devices, input gated on the TV's CEC state, CEC logical-address claiming, and a pop-one-level command from a key or pad chord. evdev-bridge: holds an interposer bridge session that switches the TV to another input and forwards local input to it over CEC. evdev-chords: passive chord observer firing configured commands, sharing the interposer's chord/device libraries.";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    flake-lib = {
      url = "github:jgus-org/flake-lib/v1";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    treefmt-nix = {
      url = "github:numtide/treefmt-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-lib,
      treefmt-nix,
    }:
    flake-lib.lib.eachDefaultLinuxSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };

        treefmtEval = treefmt-nix.lib.evalModule pkgs {
          projectRootFile = "flake.nix";
          programs.nixfmt.enable = true;
          programs.clang-format.enable = true;
        };

        evdev-tools = pkgs.stdenv.mkDerivation {
          pname = "evdev-tools";
          version = "0.1";
          src = ./.;

          nativeBuildInputs = [ pkgs.pkg-config ];
          buildInputs = [
            pkgs.systemd
            pkgs.boost
          ];
          checkInputs = [ pkgs.gtest ];

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

          doCheck = true;
          checkPhase = ''
            runHook preCheck
            ./interposer_test
            runHook postCheck
          '';

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
        };
      in
      {
        packages = {
          inherit evdev-tools;
          default = evdev-tools;
        };

        formatter = treefmtEval.config.build.wrapper;
        checks.formatting = treefmtEval.config.build.check self;
      }
    );
}
