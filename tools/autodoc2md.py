#!/usr/bin/env python3
"""autodoc2md.py — generate userdocs/mqtt-library-reference.md from the
AmigaOS-format autodoc at src/library/mqtt.doc.

src/library/mqtt.doc is the single source of truth for mqtt.library's API
reference (hand-maintained, shipped as-is in the release archive's
developer/ tree). This script turns it into a Markdown page so the same
reference shows up in the web docs (userdocs/, via mkdocs) and, since
tools/docs2guide.py picks up every userdocs page listed in its own PAGES,
in the shipped midge.guide too - one edit (to mqtt.doc) reaches all three.

The generated file is checked in like any other userdocs page (mkdocs'
docs_dir must contain it at build time, and the shared docs-deploy
workflow this project uses has no hook to run a generation step first) -
but it is GENERATED, not hand-edited. Edit src/library/mqtt.doc and rerun
`make api-reference` instead.

Usage: autodoc2md.py <mqtt.doc> <output.md>
"""

import os
import re
import sys

# Amiga autodoc section headers -> the Markdown H3 title to emit. Order of
# appearance in the source is preserved; this is just a display-name map.
SECTION_TITLES = {
    'NAME': 'Name',
    'SYNOPSIS': 'Synopsis',
    'FUNCTION': 'Function',
    'ARCHITECTURE': 'Architecture',
    'THREADING MODEL': 'Threading Model',
    'QOS SUPPORT': 'QoS Support',
    'AUTO-RECONNECT (mco_AutoReconnect)': 'Auto-Reconnect (mco_AutoReconnect)',
    'ERROR CODES': 'Error Codes',
    'INPUTS': 'Inputs',
    'RESULT': 'Result',
    'NOTES': 'Notes',
    'SEE ALSO': 'See Also',
}

# Section headers sit at a fixed 4-space indent; body text always sits
# deeper (8+ spaces) - that indentation is the real structural signal, so
# the character class allows mixed case (e.g. "AUTO-RECONNECT
# (mco_AutoReconnect)") without risking a false match against body text.
SECTION_RE = re.compile(r'^    ([A-Z][A-Za-z0-9 /()_\-]*)$')
NODE_RE = re.compile(r'^mqtt\.library/(\S+)')


def normalize_bullets(lines):
    """Re-indent any bullet list so its marker sits at column 0 ('- ') with
    continuation lines at exactly 2 spaces, regardless of how deeply the
    source autodoc nested it. Every hand-written userdocs page in this
    project uses that convention, and tools/docs2guide.py's bullet parser
    assumes it: a continuation line indented 4+ spaces is read as an
    indented code block, not list continuation (its `startswith('    ')`
    check) - a nested bullet's own natural 4-space-aligned continuation
    would otherwise silently fall out of the list."""
    out = []
    in_item = False
    for line in lines:
        stripped = line.strip()
        if not stripped:
            out.append('')
            in_item = False
            continue
        bm = re.match(r'^\s*-\s+(.*)$', line)
        if bm:
            out.append('- ' + bm.group(1))
            in_item = True
        elif in_item:
            out.append('  ' + stripped)
        else:
            out.append(line)
    return out


def dedent_block(lines):
    """Strip the common leading whitespace across a section's non-blank
    lines, preserving relative indentation (list nesting, wrapped
    continuation lines)."""
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    if not lines:
        return lines
    indents = [len(l) - len(l.lstrip(' ')) for l in lines if l.strip()]
    base = min(indents)
    return [l[base:] if len(l) >= base else l for l in lines]


def convert_node(node_text):
    lines = node_text.split('\n')
    header_line = next((l for l in lines if l.strip()), None)
    if header_line is None:
        return None
    m = NODE_RE.match(header_line)
    if not m:
        return None
    name = m.group(1)
    title = 'Background' if name == '--background--' else name

    rest = lines[lines.index(header_line) + 1:]
    sections = []
    heading, body = None, []
    for line in rest:
        sm = SECTION_RE.match(line)
        if sm:
            if heading:
                sections.append((heading, body))
            heading, body = sm.group(1).strip(), []
        else:
            body.append(line)
    if heading:
        sections.append((heading, body))

    out = ['## %s' % title, '']
    for heading, body in sections:
        body = dedent_block(body)
        if not body:
            continue
        if heading not in ('SYNOPSIS', 'ERROR CODES'):
            body = normalize_bullets(body)
        display = SECTION_TITLES.get(heading, heading.title())
        out.append('### %s' % display)
        out.append('')
        if heading in ('SYNOPSIS', 'ERROR CODES'):
            # ERROR CODES nests a columnar constant table inside its own
            # bullets - code-fencing the whole section preserves both the
            # alignment and each entry as its own line, rather than
            # feeding it through docs2guide's bullet-continuation logic
            # (which would merge every entry into one run-on paragraph).
            out.append('```')
            out.extend(body)
            out.append('```')
        else:
            out.extend(body)
        out.append('')
    return '\n'.join(out)


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: autodoc2md.py <mqtt.doc> <output.md>')
    src_path, out_path = sys.argv[1], sys.argv[2]

    text = open(src_path, encoding='latin-1').read()
    # Node separator in a classic AmigaOS autodoc is a form feed (0x0C);
    # the first "node" is the TABLE OF CONTENTS, not a real section.
    nodes = text.split('\x0c')[1:]

    head = [
        '# mqtt.library API reference',
        '',
        '<!-- Generated by tools/autodoc2md.py from src/library/mqtt.doc - '
        'edit that file and run `make api-reference`, not this one. -->',
        '',
        'This page is the same reference shipped as `mqtt.doc` in the '
        'release archive\'s `developer/` directory, generated here so it '
        'also shows up in the web docs. See [mqtt.library](mqtt-library.md) '
        'for the narrative guide (installing it, the threading model, '
        'auto-reconnect) - this page is the function-by-function reference.',
        '',
    ]
    body = []
    count = 0
    for node in nodes:
        converted = convert_node(node)
        if converted:
            body.append(converted)
            count += 1

    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(head + body))
    print('%s: %d nodes' % (out_path, count))


if __name__ == '__main__':
    main()
