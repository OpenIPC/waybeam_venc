# json_cli — vendored from waybeam-hub

`json_cli` is a style-preserving JSON config get/set/del tool. We vendor it
here (rather than reaching across submodule boundaries during a `cycle`)
because the Maruko deploy scripts use it to read and patch `/etc/venc.json`
on the target device.

## Source of truth

The canonical version lives in
`../waybeam-hub/tools/{json_cli.c,jsmn.h}`. Both files are byte-identical
copies — update by re-running:

```sh
cp ../waybeam-hub/tools/json_cli.c tools/json_cli/json_cli.c
cp ../waybeam-hub/tools/jsmn.h     tools/json_cli/jsmn.h
```

…then rebuild with `make json_cli SOC_BUILD=maruko`.

## Build

The top-level `Makefile` builds `out/<soc>/json_cli` using the SOC's
toolchain:

```sh
make json_cli SOC_BUILD=maruko    # → out/maruko/json_cli
make json_cli SOC_BUILD=star6e    # → out/star6e/json_cli
```

`scripts/maruko_direct_deploy.sh push-libs` (and `cycle`/`full`) pushes
the matching binary to `/usr/bin/json_cli` on the device.
