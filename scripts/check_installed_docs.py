#!/usr/bin/env python3
"""检查安装文档清单及 Markdown 相对文件链接。

只搜索重定位后的安装文档树，不从源码树补齐缺失目标。
跳过围栏代码块和行内代码，检查行内链接、图片链接及引用定义。
允许链接包含片段，但不验证标题锚点的拼写。
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
            raise ValueError('文档路径无效或重复：' + entry)
        entries.append(entry)
    if not entries:
        raise ValueError('文档清单为空')
    return entries


def markdown_targets(text):
    # 围栏代码块包含命令或示例，不是渲染后的文档导航。
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
    # 逐字符读取目标，不用正则排除括号：Markdown 的裸链接允许
    # 配对括号，也允许转义后的括号。
    patterns = ((r'(?<!\\)\]\(\s*', 0),
                (r'^\s{0,3}\[[^\]\n]+\]:\s*', re.M))
    for pattern, flags in patterns:
        for match in re.finditer(pattern, text, flags):
            target = markdown_destination(text, match.end())
            if target:
                yield re.sub(r'\\([\\() ])', r'\1', target)


def markdown_destination(text, start):
    if start >= len(text):
        raise ValueError('Markdown 链接缺少目标')
    angle = text[start] == '<'
    pos = start + 1 if angle else start
    begin = pos
    depth = 0
    while pos < len(text):
        char = text[pos]
        if char == '\\' and pos + 1 < len(text):
            pos += 2
            continue
        if angle:
            if char == '>':
                return text[begin:pos]
            if char == '\n':
                break
        else:
            if char == '(':
                depth += 1
            elif char == ')':
                if depth == 0:
                    return text[begin:pos]
                depth -= 1
            elif char.isspace():
                if depth == 0:
                    return text[begin:pos]
                break
        pos += 1
    if not angle and depth == 0:
        return text[begin:pos]
    raise ValueError('Markdown 链接目标的括号或尖括号不匹配：' + text[begin:pos])


def verify(doc_root, manifest):
    doc_root = doc_root.resolve()
    entries = read_manifest(manifest)
    markdown = {entry for entry in entries if entry.endswith('.md')}
    for entry in entries:
        target = (doc_root / entry).resolve()
        if doc_root not in target.parents or not target.is_file():
            raise ValueError('安装文档缺失或超出文档树：' + entry)
    installed = {path.relative_to(doc_root).as_posix() for path in doc_root.rglob('*.md')}
    if installed != markdown:
        raise ValueError('安装了清单以外的 Markdown 文档：' +
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
                raise ValueError('{}: 相对链接目标缺失或超出文档树：{}'.format(entry, raw))
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
        parser.exit(1, '安装文档验证失败：{}\n'.format(error))


if __name__ == '__main__':
    main()
