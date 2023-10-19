{
  outputs = { nixpkgs, ... }: let
    attrs = k: f: builtins.listToAttrs
      (map (n: { name = n; value = f n; }) k);
  in {
    packages = attrs [ "x86_64-linux" "aarch64-linux" ]
      (system: let
        pkgs = nixpkgs.legacyPackages.${system};
      in {
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
