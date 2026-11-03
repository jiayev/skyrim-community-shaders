# Shader cache

Every shader Community Shaders builds, with FXC or DXC, goes through one store: `org::services::ShaderStore`
(`extern/ORGModuleServices`). It lives in `Data/ShaderCache/v2`. The rule it follows is simple: a saved shader is used
only when every file it read last time still has the same contents.

## How one shader is resolved

1. **Look up the variant** by its key: the root name, entry point, target, defines in order, final compiler
   arguments and compiler identity.
2. **Check its files.** The record lists each file the compiler opened, and each path it tried that did not exist.
   If they all still match and the root is unchanged, read the artifact. This is a hit, and it does not preprocess.
3. **Otherwise preprocess** and hash the text. If an artifact exists for that text, use it and update the record. This
   is "absorbed". It covers comment edits, inactive code, reverted edits, and variants that compile to the same thing.
4. **Otherwise compile** and save.

Debug builds skip step 3's sharing. They embed source text, so any source change must rebuild them.

File times are never used. Each file is read and hashed once per generation, and a stat costs about as much as
the read. A hot reload or a shader clear starts a new generation.

## Reading the log

Each batch logs one line:

```
Shader store after shader compilation: 4719 requests: 4549 hits, 170 absorbed, 0 compiled, 0 failed (0 remembered),
0 not saved. 170 preprocessed, 190 source files read. Misses: 0 no record, 0 root changed, 170 dependency changed,
0 artifact invalid. First changed file: shaders/watereffects/waterparallax.hlsli
```

| Field | Meaning |
|---|---|
| `no record` | The variant is new: a new define set, flag or compiler, or the index was lost. |
| `root changed` | A generated root's bytes differ, such as a DCLF rewrite. |
| `dependency changed` | A file it read changed, or a path it tried now exists. The first such file is named. |
| `artifact invalid` | The artifact is missing, corrupt or evicted. |
| `not saved` | The compile worked, but its file could not be written. |
| `remembered` | The same inputs failed before, so nothing ran. Fixing any file it read clears this. |

A start with no changes should show all hits and `0 preprocessed`. If you see `compiled` on every start, this line
says why.

## On disk

- `index.a`, `index.b`: the variant records. Saved in turn, each with a hash, so a torn write falls back to the other.
  Losing both costs preprocessing, never compiling.
- `cas/xx/<key>.bin`: artifacts, named by what they contain. Each has a header with a hash, and is written without a
  rename, because renames are what fail under MO2's usvfs. A damaged file costs one rebuild.

Formats in the artifact header: DXIL 0, SPIR-V 1, DXBC 2. `tools/aftermath-decode --spirv` reads this folder.

## Eviction

At startup, if the folder is over the size limit (`Shader Store Max MB`, 4 GB by default): artifacts no record uses go
first, then the longest unused. Anything used in the last three starts stays even over the limit, with a warning,
because evicting live shaders would rebuild them on every start.

## Disk cache off

Nothing is read, written or deleted. As before, the game also does not precompile at startup in this mode, so loads do
not wait and shaders compile as they are drawn.

## Tests

`extern/ORGModuleServices/tests/ShaderStore` covers the engine with a fake compiler that counts its calls.
`extern/ORGModuleServices/tests/Services` covers the same behaviour through `ShaderCompiler` with real DXC.
Both build standalone: `cmake -S tests/ShaderStore -B build/store && cmake --build build/store`.
