#!/usr/bin/env python3
"""Check the installed document manifest and all Markdown relative file links.

Only the relocated documentation tree is searched, never the source tree.
Inline/image links and reference definitions are checked outside fenced/inline code;
fragments are allowed but heading-anchor spelling is not validated.
"""
import argparse
import json
import re
from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit


def read_manifest(path):
    entries = []
    for line in path.read_text(encoding='utf-8').splitlines():
        entry = line.strip()
        if not entry or entry.startswith('#'):
            continue
        relative = PurePosixPath(entry)
        if (relative.is_absolute() or '..' in relative.parts or '\\' in entry or
                relative.as_posix() != entry or entry in entries):
            raise ValueError('invalid or duplicate document path: ' + entry)
        entries.append(entry)
    if not entries:
        raise ValueError('empty document manifest')
    return entries


def markdown_targets(text):
    # Fences contain command/examples, not rendered document navigation.
    lines = []
    fence = None
    for line in text.splitlines():
        marker = re.match(r'^\s{0,3}(`{3,}|~{3,})', line)
        if fence:
            if marker and marker[1][0] == fence[0] and len(marker[1]) >= len(fence):
                fence = None
            continue
        if marker:
            fence = marker[1]
            continue
        lines.append(line)
    text = '\n'.join(lines)
    text = re.sub(r'<!--.*?-->', '', text, flags=re.S)
    text = re.sub(r'(`+).*?\1', '', text, flags=re.S)
    # Current docs use standard inline links and may also use references.
    destination = r'(<[^>\n]+>|(?:\\.|[^\s()])+)'
    inline = r'!?\[[^\]\n]*\]\(\s*' + destination + r'(?:\s+["\'][^\n]*["\'])?\s*\)'
    reference = r'^\s{0,3}\[[^\]\n]+\]:\s*' + destination
    for pattern, flags in ((inline, 0), (reference, re.M)):
        for match in re.finditer(pattern, text, flags):
            target = match[1]
            if target.startswith('<'):
                target = target[1:-1]
            yield re.sub(r'\\([\\() ])', r'\1', target)


def verify(doc_root, manifest):
    doc_root = doc_root.resolve()
    entries = read_manifest(manifest)
    markdown = {entry for entry in entries if entry.endswith('.md')}
    for entry in entries:
        target = (doc_root / entry).resolve()
        if doc_root not in target.parents or not target.is_file():
            raise ValueError('missing or out-of-tree installed document: ' + entry)
    installed = {path.relative_to(doc_root).as_posix() for path in doc_root.rglob('*.md')}
    if installed != markdown:
        raise ValueError('unexpected installed Markdown documents: ' +
                         ', '.join(sorted(installed - markdown)))
    links = 0
    for entry in sorted(markdown):
        document = doc_root / entry
        for raw in markdown_targets(document.read_text(encoding='utf-8')):
            url = urlsplit(raw)
            if url.scheme or url.netloc or not url.path:
                continue
            target = (document.parent / unquote(url.path)).resolve()
            if doc_root not in target.parents or not target.is_file():
                raise ValueError('{}: missing or out-of-tree relative link: {}'.format(entry, raw))
            links += 1
    return {'passed': True, 'documents': len(entries),
            'markdown_documents': len(markdown), 'relative_file_links': links}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--doc-root', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.doc_root, args.manifest), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, 'installed documentation verification failed: {}\n'.format(error))


if __name__ == '__main__':
    main()
