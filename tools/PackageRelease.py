"""Package the built Animate DLLs and example skin for Rainmeter's installer."""
from pathlib import Path
import argparse
import configparser
import re
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def package(destination):
    resource = (ROOT / 'PluginAnimate.rc').read_text(encoding='cp1252')
    version = re.search(r'VALUE "FileVersion", "([\d.]+)"', resource)[1]
    short_version = '.'.join(version.split('.')[:2])
    destination.mkdir(parents=True, exist_ok=True)
    target = destination / f'Animate_{short_version}.rmskin'
    metadata = ('[rmskin]\r\nName=Animate\r\nAuthor=JSMorley\r\n'
                f'Version={version}\r\nLoadType=Skin\r\nLoad=Animate\\Animate.ini\r\n'
                'MinimumRainmeter=4.5\r\n')
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('RMSKIN.ini', metadata.encode('utf-16'))
        for platform, folder in (('32bit', 'x32'), ('64bit', 'x64')):
            archive.write(ROOT / 'Releases' / folder / 'Animate.dll', f'Plugins/{platform}/Animate.dll')
        skin = ROOT / 'examples' / 'Animate'
        for path in sorted(skin.rglob('*')):
            if path.is_file() and 'Temp' not in path.relative_to(skin).parts:
                archive.write(path, 'Skins/Animate/' + path.relative_to(skin).as_posix())
        for source, filename in (('README.md','README.md'), ('LICENSE','LICENSE.txt'),
                                 ('THIRD-PARTY-NOTICES.md','THIRD-PARTY-NOTICES.md'),
                                 ('Releases/WEBP-NOTICES.txt','WEBP-NOTICES.txt')):
            archive.write(ROOT / source, 'Skins/Animate/@Resources/Docs/' + filename)
    archive_size = target.stat().st_size
    with target.open('ab') as stream:
        stream.write(struct.pack('<Q', archive_size) + b'\0RMSKIN\0')
    data = target.read_bytes()
    assert data[-8:] == b'\0RMSKIN\0' and struct.unpack('<Q',data[-16:-8])[0] == len(data)-16
    with zipfile.ZipFile(target) as archive:
        assert archive.testzip() is None
        configuration = configparser.ConfigParser()
        configuration.read_string(archive.read('RMSKIN.ini').decode('utf-16'))
        assert configuration['rmskin']['Version'] == version
        assert configuration['rmskin']['MinimumRainmeter'] == '4.5'
        for platform, folder in (('32bit','x32'), ('64bit','x64')):
            assert archive.read(f'Plugins/{platform}/Animate.dll') == (ROOT/'Releases'/folder/'Animate.dll').read_bytes()
    print(f'Created and verified {target} ({version}, {len(data):,} bytes)')
    return target

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'output'/'release')
    package(parser.parse_args().output.resolve())
