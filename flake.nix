{
  outputs = { nixpkgs, ... }: let
    systems = fn: nixpkgs.lib.mapAttrs (_: fn) nixpkgs.legacyPackages;
  in {
    packages = systems (pkgs: {
      default = pkgs.stdenv.mkDerivation {
        name = "redact-pdf";
        src = ./.;
        buildInputs = [ pkgs.qpdf ];
        buildPhase = "./build";
        installPhase = ''
          mkdir -p "$out/bin"
          mv redact-pdf "$out/bin"
        '';
      };
    });
  };
}
