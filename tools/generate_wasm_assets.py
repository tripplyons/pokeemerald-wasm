#!/usr/bin/env python3
import pathlib
import re
import shlex
import subprocess

GFX = pathlib.Path('tools/gbagfx/gbagfx')
SOURCE_SUFFIXES = {'.c', '.h', '.inc'}
INCGFX = re.compile(r'INCGFX_U(?:8|16|32)\(\s*"([^"]+)"\s*,\s*"([^"]+)"(?:\s*,\s*"([^"]*)")?\s*\)')
INCBIN = re.compile(r'INCBIN_U(?:8|16|32)\(([^)]*)\)')


def encoded_args(args):
    return ''.join(c if c.isalnum() else '_' for c in args)


def source_files():
    ignored = {'.git', 'build'}
    for path in pathlib.Path('.').rglob('*'):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        if ignored.intersection(path.parts):
            continue
        yield path


def run_gbagfx(input_path, output_path, options=()):
    output = pathlib.Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        return
    subprocess.run([str(GFX), input_path, output_path, *options], check=True)


def ensure_make_target(target):
    if pathlib.Path(target).exists():
        return
    subprocess.run(['make', 'NODEP=1', 'SETUP_PREREQS=1', target], check=True)


def generate_incgfx(source, extension, args):
    final = pathlib.Path('build/assets') / f'{source}{encoded_args(args)}{extension}'
    options = shlex.split(args) if args else []
    ensure_make_target(source)

    if extension in ('.lz', '.rl'):
        run_gbagfx(source, str(final))
        return final

    if extension.endswith(('.lz', '.rl')):
        intermediate = pathlib.Path(str(final)[:-3])
        run_gbagfx(source, str(intermediate), options)
        run_gbagfx(str(intermediate), str(final))
        return final

    run_gbagfx(source, str(final), options)
    return final


def generate_incbin(path):
    ensure_make_target(path)


def main():
    incgfx_seen = set()
    incbin_seen = set()
    for path in source_files():
        text = path.read_text(errors='ignore')
        for match in INCGFX.finditer(text):
            item = match.group(1), match.group(2), match.group(3) or ''
            if item not in incgfx_seen:
                incgfx_seen.add(item)
                generate_incgfx(*item)
        for match in INCBIN.finditer(text):
            for item in re.findall(r'"([^"]+)"', match.group(1)):
                if item not in incbin_seen:
                    incbin_seen.add(item)
                    generate_incbin(item)

    print(f'generated {len(incgfx_seen)} INCGFX assets and {len(incbin_seen)} INCBIN assets')


if __name__ == '__main__':
    main()
