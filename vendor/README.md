# vendor/

Reference clones of third-party OP-1 firmware projects. Everything in this
folder except this README is gitignored because these projects are not ours
to redistribute.

Expected layout:

    vendor/hundekraft/   # hundekraft custom firmware repo
    vendor/opus/         # opus custom firmware repo

The librarian reads these only to understand patch formats and drive layouts
when building the hundekraft and opus device profiles.
