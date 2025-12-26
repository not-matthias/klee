{
  description = "KLEE Symbolic Virtual Machine";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # Use LLVM 18 (adjust based on compatibility needs)
        llvmPackages = pkgs.llvmPackages_18;

        pythonEnv = pkgs.python3.withPackages (ps: with ps; [
          tabulate
          lit
        ]);

      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "klee";
          version = "3.3-pre";

          src = ./.;

          nativeBuildInputs = [
            pkgs.cmake
            llvmPackages.llvm.dev
          ];

          buildInputs = [
            llvmPackages.llvm
            llvmPackages.clang
            pkgs.z3
            pkgs.stp
            pkgs.cryptominisat
            pkgs.sqlite
            pkgs.gperftools
            pkgs.gtest
            pythonEnv
          ];

          cmakeFlags = [
            "-DLLVM_DIR=${llvmPackages.llvm.dev}/lib/cmake/llvm"
            "-DLLVMCC=${llvmPackages.clang}/bin/clang"
            "-DLLVMCXX=${llvmPackages.clang}/bin/clang++"
            "-DENABLE_SOLVER_Z3=ON"
            "-DENABLE_SOLVER_STP=ON"
            "-DENABLE_POSIX_RUNTIME=ON"
            "-DENABLE_KLEE_UCLIBC=ON"
            "-DENABLE_KLEE_ASSERTS=ON"
            "-DENABLE_UNIT_TESTS=ON"
            "-DENABLE_SYSTEM_TESTS=ON"
            "-DLIT_ARGS=-v"
          ];

          # Disable tests during build by default (can be slow)
          doCheck = false;

          meta = with pkgs.lib; {
            description = "Symbolic execution engine built on top of LLVM";
            homepage = "https://klee-se.org/";
            license = licenses.ncsa;
            platforms = platforms.linux;
            maintainers = [ ];
          };
        };

        devShells.default = pkgs.mkShell {
          buildInputs = [
            # Build tools
            pkgs.cmake
            pkgs.ninja
            pkgs.ccache

            # LLVM toolchain
            llvmPackages.llvm
            llvmPackages.clang
            llvmPackages.clang-tools
            llvmPackages.lld

            # Solvers
            pkgs.z3
            pkgs.stp
            pkgs.cryptominisat

            # Other dependencies
            pkgs.sqlite
            pkgs.gperftools
            pkgs.gtest

            # Python tools for testing
            pythonEnv

            # Development utilities
            pkgs.git
            pkgs.gdb
            pkgs.valgrind
          ];

          shellHook = ''
            echo "KLEE development environment"
            echo "LLVM version: ${llvmPackages.llvm.version}"
            echo ""
            echo "Build with:"
            echo "  mkdir build && cd build"
            echo "  cmake -G Ninja .."
            echo "  ninja"
            echo ""
            echo "Run tests with:"
            echo "  cd build && lit test/"
          '';

          # Set up environment variables
          LLVM_DIR = "${llvmPackages.llvm.dev}/lib/cmake/llvm";
          LLVMCC = "${llvmPackages.clang}/bin/clang";
          LLVMCXX = "${llvmPackages.clang}/bin/clang++";
        };
      }
    );
}
