# Printing

Nordstjernen lays a page out for paper and hands the sheets to the
operating system's own print dialog. No printing UI is written per
platform and no dependency is added: the sheets go to
`GtkPrintOperation`, which is CUPS on Linux, the Win32 printer dialog on
Windows, and the Cocoa print panel on macOS.

## Printing a page

`Ctrl+P`, or **Print…** in the **☰** toolbar menu, opens the system
print dialog with the current page paginated behind it. Paper size,
orientation, printer and copies are the dialog's business; what
Nordstjernen decides is where each sheet begins and ends.

Printing works in every process mode. Under `--single-process` (or
`NS_SINGLE_PROCESS=1`) the sheets stay cairo **recording surfaces** and
reach the printer as vectors. In the default process-per-tab mode a
recording surface crosses no process boundary, so the renderer rasterises
each sheet at `NS_PRINT_RASTER_SCALE` device pixels per CSS pixel, writes
it beside the page-export files in the runtime directory, and the shell
loads the sheets back and prints them. The pagination is identical either
way; only the sheets' resolution differs.

To get a file rather than a printer, from any mode and with no printer
configured:

```sh
nordstjernen --dump=print:out.pdf https://example.org/
```

That renders the same pagination to a multi-page PDF. It is the quickest
way to check what a page will do on paper.

## The CSS a printer reads

Pagination is ordinary CSS, so a page's own print stylesheet decides most
of the result.

**`@media print`** matches while laying out for paper. The media type is
the one being laid out for rather than a hardcoded `screen`, so a print
stylesheet applies — and only then.

**`@page`** sets the sheet. It takes a paper name (`A4`, `letter`,
`legal`, `ledger`, and the A and B series), one length for a square
sheet or two for width and height, or `portrait`/`landscape`, along with
the margins:

```css
@page {
  size: A4 portrait;
  margin: 18mm 14mm;
}
```

**Breaks.** `break-before`, `break-after` and `break-inside` decide where
a sheet may end, with the legacy `page-break-*` spellings mapping onto
them and `always` becoming `page`:

```css
h2            { break-after: avoid; }
figure, table { break-inside: avoid; }
.chapter      { break-before: page; }
```

A sheet is cut at a forced break when one falls before the sheet is full.
Otherwise the cut is pulled up above any box it would split — which means
every leaf box, every line of a paragraph, and anything asking for
`break-inside: avoid`. The on-screen layout is restored afterwards, so
printing leaves the page as it found it.

## Checking the result

`data/render-tests/print-pagination.html` is the fixture the behaviour is
kept honest against:

```sh
./builddir/src/gtk/nordstjernen --dump=print:/tmp/pagination.pdf \
    data/render-tests/print-pagination.html
```

It comes out as three A4 sheets, with every card whole, the
`@media print` paragraph swapped in for the screen one, and the forced
break starting sheet three.

## See also

- [Controls.md](Controls.md) — the keyboard shortcuts.
- [CSS-compatibility.md](CSS-compatibility.md) — the paged-media rows.
- [Rendering.md](Rendering.md) — how the same box tree reaches the screen.
- [single-process-mode.md](single-process-mode.md) — what `--single-process` changes.
