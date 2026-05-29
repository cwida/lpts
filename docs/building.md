# Building LPTS

## Prerequisites

- **git** with submodule support
- **cmake**
- **ninja** for faster builds
- A C++ compiler supported by DuckDB

## Clone

```bash
git clone --recurse-submodules https://github.com/cwida/lpts.git
cd lpts
```

If you already cloned without submodules, run:

```bash
git submodule update --init --recursive
```

## Build

```bash
GEN=ninja make        # release build
make                  # release build without ninja
make debug            # debug build
```

The release build produces:

```text
build/release/duckdb
build/release/test/unittest
build/release/extension/lpts/lpts.duckdb_extension
build/release/extension/lpts/lpts_sqlstorm_benchmark
```

## Load The Extension

Use community-extension loading for installed releases:

```sql
INSTALL lpts FROM community;
LOAD lpts;
```

Use the local build path while developing:

```bash
build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/lpts/lpts.duckdb_extension';
```

## Format Code

```bash
make format-fix
```

## CLion

Open `duckdb/CMakeLists.txt` as the CLion project, then change the project root
to the LPTS repository root.

For each CMake profile, set the build directory to `../build/<profile>` and add
the extension configuration flag:

```text
-DDUCKDB_EXTENSION_CONFIGS=<path_to_lpts>/extension_config.cmake
```

Use the `unittest` target for SQLLogicTests. To run only LPTS tests, pass a
single test file such as:

```text
test/sql/select.test
```

## Updating DuckDB

The DuckDB version is pinned via the `duckdb` and `extension-ci-tools`
submodules. To update to a new release:

```bash
cd duckdb && git fetch --tags && git checkout v1.X.0
cd ../extension-ci-tools && git fetch && git checkout origin/v1.X.0
cd ..
git add duckdb extension-ci-tools
git commit -m "Bump DuckDB to v1.X.0"
```

Replace `v1.X.0` with the target version tag.
