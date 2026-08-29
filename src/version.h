#ifndef MIDGE_VERSION_H
#define MIDGE_VERSION_H

/* Single source of truth for the release version.
 *
 * A release PR bumps MIDGE_VERSION and MIDGE_VERSION_DATE here and the
 * matching `Version:` field in midge.readme; the tag-driven release workflow
 * (.github/workflows/release.yml) refuses a v<tag> that does not match both.
 *
 * MIDGE_VERSION_DATE is the AmigaOS $VER date, <dd>.<mm>.<yyyy> per
 * https://wiki.amigaos.net/wiki/Version_Strings */
#define MIDGE_VERSION      "0.1"
#define MIDGE_VERSION_DATE "29.08.2026"

/* Embedded AmigaOS version string, findable by the shell `Version` command.
 * The leading "\0" guards against an adjacent string in the binary running
 * into ours; `used` keeps the otherwise-unreferenced constant out of the
 * optimiser's reach. */
#define MIDGE_VERSTAG(name) \
    static const char verstag[] __attribute__((used)) = \
        "\0$VER: " name " " MIDGE_VERSION " (" MIDGE_VERSION_DATE ")";

#endif
