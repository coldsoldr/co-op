# vendor/

Reference clones of third-party OP-1 firmware projects. Everything in this
folder except this README is gitignored because these projects are not ours
to redistribute.

Expected layout:

    vendor/hundekraft/   # hundekraft custom firmware repo
    vendor/opus/         # opus custom firmware repo

The librarian reads these only to understand patch formats and drive layouts
when building the hundekraft and opus device profiles.

Some tests read files from here and skip when they are missing (CI has no
vendor folder). Point them at other copies with:

    CO_OP_PRESETS_ZIP     hundekraft/op1-workshop-main/variants/7ff/presets.zip
                          (152 hundekraft factory presets)
    CO_OP_FIRMWARE_TABLE  hundekraft/op1-workshop-main/base/OP1_vdk.ldr.insert.bin
                          (stock name tables)

for example `CO_OP_PRESETS_ZIP=/path/presets.zip pnpm test`.
