# SPDX-License-Identifier: GPL-3.0-only
"""Package the validated translation build and source. Requires Python 3.10+."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath


BASELINE = "b1a8e22577c9e35064315d279fc33ba6b67c5bef"
UPSTREAM_URL = "https://github.com/sumatrapdfreader/sumatrapdf"
PORTABLE_NAME = "SumatraPDF-Translate-x64"
SOURCE_NAME = "SumatraPDF-Translate-source"
READ_SIZE = 1024 * 1024
MAX_EVIDENCE_SIZE = 2 * READ_SIZE
DOS_HEADER_SIZE = 64
DOS_PE_OFFSET = 60
PE_HEADER_SIZE = 26
PE_MACHINE_OFFSET = 4
PE_FLAGS_OFFSET = 22
PE_MAGIC_OFFSET = 24
PE_AMD64 = 0x8664
PE32_PLUS = 0x20B
PE_DLL = 0x2000
EXTRA_SOURCE = (
    "src/LlmRequestOptions.h",
    "src/LlmRequestOptions.cpp",
    "src/LlmTranslate.h",
    "src/LlmTranslate.cpp",
    "src/TranslatePopup.h",
    "src/TranslatePopup.cpp",
    "src/TranslateAnchor.h",
    "src/TranslateAnchor.cpp",
    "src/TranslateParams.h",
    "src/TranslateParams.cpp",
    "tests/ad-hoc-llm-translate.ts",
    "tests/ad-hoc-translate-settings.ts",
    "tests/ad-hoc-translate-options.ts",
    "tests/translate-test-controls.ts",
    "tests/ad-hoc-translate-appearance.ts",
    "tests/ad-hoc-translate-style.ts",
    "tests/ad-hoc-translate-anchor.ts",
    "tools/build-translation.ps1",
    "tools/package-translation.py",
    "docs/selection-translation.md",
    "docs/plans/2026-11-17-selection-translation.md",
    "docs/plans/2026-11-17-translation-options.md",
    "docs/plans/2026-11-17-translation-appearance.md",
    "docs/plans/2026-11-17-translation-card.md",
    "docs/plans/2026-11-17-translation-anchor.md",
)
SETTINGS = """CheckForUpdates = false
RestoreSession = false
TranslatePopupEnabled = true
TranslatePopupUrl =
TranslatePopupModel =
TranslatePopupApiKey =
TranslatePopupTemperature = 1
TranslatePopupStream = true
TranslatePopupThinking = off
TranslatePopupLangFrom = auto
TranslatePopupLangTo = zh-CN
TranslatePopupCustomParams = {}
TranslatePopupFont = Microsoft YaHei UI
TranslatePopupFontSize = 11
TranslatePopupLineSpacing = 100
TranslatePopupBold = false
TranslatePopupPosition = auto
TranslatePopupAutoSize = true
TranslatePopupWidth = 460
TranslatePopupHeight = 350
TranslatePopupMaxHeight = 70
"""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(READ_SIZE), b""):
            digest.update(block)
    return digest.hexdigest()


def bytes_hash(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def within(path: Path, parent: Path) -> Path:
    resolved = path.resolve()
    if not resolved.is_relative_to(parent.resolve()):
        raise ValueError(f"路径超出允许目录: {path}")
    return resolved


def source_path(root: Path, relative: str) -> Path:
    pure = PurePosixPath(relative)
    if pure.is_absolute() or ".." in pure.parts or "\\" in relative:
        raise ValueError(f"无效源码路径: {relative}")
    path = within(root.joinpath(*pure.parts), root)
    if not path.is_file():
        raise ValueError(f"源码文件不存在: {relative}")
    return path


def run_git(git: str, args: list[str], **kwargs) -> subprocess.CompletedProcess:
    # The inventory hashes raw bytes, so preserve CRLF when generating and checking patches.
    result = subprocess.run(
        [git, "--no-optional-locks", "-c", "core.quotePath=false", "-c", "core.autocrlf=false", *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        **kwargs,
    )
    return result


def tracked_files(git: str, upstream: Path) -> dict[str, str]:
    head = run_git(git, ["-C", str(upstream), "rev-parse", "HEAD"])
    if head.returncode or head.stdout.decode().strip() != BASELINE:
        raise ValueError("上游检出版本与固定基线不一致")
    status = run_git(git, ["-C", str(upstream), "status", "--porcelain=v1", "--untracked-files=no"])
    if status.returncode:
        raise ValueError(status.stderr.decode("utf-8", "replace"))
    if status.stdout:
        raise ValueError("上游检出的暂存区或已跟踪文件与固定基线不一致")
    result = run_git(git, ["-C", str(upstream), "ls-tree", "-r", "-z", "--full-tree", BASELINE])
    if result.returncode:
        raise ValueError(result.stderr.decode("utf-8", "replace"))
    files = {}
    for entry in result.stdout.split(b"\0"):
        if not entry:
            continue
        metadata, name = entry.split(b"\t", 1)
        mode, kind, _ = metadata.decode("ascii").split()
        if kind != "blob" or mode not in {"100644", "100755"}:
            raise ValueError(f"不支持的源码条目: {name!r} ({mode} {kind})")
        files[name.decode("utf-8")] = mode
    return files


def inventory(root: Path, upstream: Path, tracked: dict[str, str]) -> tuple[list[dict], list[str]]:
    files = []
    changed = []
    for relative in sorted(set(tracked).union(EXTRA_SOURCE)):
        path = source_path(root, relative)
        digest = sha256(path)
        new = relative not in tracked
        if new or sha256(source_path(upstream, relative)) != digest:
            changed.append(relative)
        files.append({"path": relative, "bytes": path.stat().st_size, "sha256": digest,
                      "mode": tracked.get(relative, "100644")})
    return files, changed


def make_patch(root: Path, upstream: Path, git: str, tracked: dict[str, str], changed: list[str]) -> bytes:
    patches = []
    for relative in changed:
        old = str(source_path(upstream, relative)) if relative in tracked else "/dev/null"
        new = str(source_path(root, relative))
        result = run_git(git, ["diff", "--no-index", "--binary", "--no-color", "--no-ext-diff",
                               "--no-textconv", "--", old, new], cwd=root)
        if result.returncode not in {0, 1}:
            raise ValueError(result.stderr.decode("utf-8", "replace"))
        header = True
        for line in result.stdout.splitlines(keepends=True):
            if line.startswith((b"@@", b"GIT binary patch")):
                header = False
            if header and line.startswith(b"diff --git "):
                line = f"diff --git a/{relative} b/{relative}\n".encode()
            elif header and line.startswith(b"--- "):
                line = (f"--- a/{relative}\n" if relative in tracked else "--- /dev/null\n").encode()
            elif header and line.startswith(b"+++ "):
                line = f"+++ b/{relative}\n".encode()
            patches.append(line)
    patch = b"".join(patches)
    check = run_git(git, ["-C", str(upstream), "apply", "--check", "--whitespace=nowarn", "-"], input=patch)
    if check.returncode:
        raise ValueError("补丁检查失败:\n" + check.stderr.decode("utf-8", "replace"))
    return patch


def pe_info(path: Path) -> dict:
    with path.open("rb") as stream:
        dos = stream.read(DOS_HEADER_SIZE)
        if len(dos) < DOS_HEADER_SIZE or dos[:2] != b"MZ":
            raise ValueError("发布文件不是 Windows PE 可执行文件")
        offset = struct.unpack_from("<I", dos, DOS_PE_OFFSET)[0]
        if offset > path.stat().st_size - PE_HEADER_SIZE:
            raise ValueError("发布文件的 PE 头无效")
        stream.seek(offset)
        header = stream.read(PE_HEADER_SIZE)
    if header[:4] != b"PE\0\0":
        raise ValueError("发布文件缺少 PE 签名")
    machine, sections = struct.unpack_from("<HH", header, PE_MACHINE_OFFSET)
    characteristics = struct.unpack_from("<H", header, PE_FLAGS_OFFSET)[0]
    magic = struct.unpack_from("<H", header, PE_MAGIC_OFFSET)[0]
    if machine != PE_AMD64 or not sections or magic != PE32_PLUS or characteristics & PE_DLL:
        raise ValueError("发布文件必须是 x64 PE32+ EXE")
    return {"format": "PE32+", "machine": "AMD64", "sections": sections}


def redact(value):
    if isinstance(value, dict):
        return {
            key: "[redacted]" if re.search(r"apikey|authorization|password|secret|credential|accesstoken",
                                           re.sub(r"[^a-z]", "", str(key).lower())) else redact(item)
            for key, item in value.items()
        }
    if isinstance(value, list):
        return [redact(item) for item in value]
    if isinstance(value, str):
        return re.sub(r"(?i)\bBearer\s+[^\s\"']+", "Bearer [redacted]", value)
    return value


def read_evidence(root: Path, directories: list[Path]) -> list[dict]:
    evidence = []
    seen = set()
    for directory in directories:
        directory = within(directory, root / "out")
        if not directory.is_dir():
            raise ValueError(f"验证目录不存在: {directory}")
        for path in sorted(directory.rglob("*.json")):
            path = within(path, directory)
            relative = path.relative_to(root).as_posix()
            if relative in seen or any("appdata" in part.lower() or "cache" in part.lower() for part in path.parts):
                continue
            seen.add(relative)
            if path.stat().st_size > MAX_EVIDENCE_SIZE:
                raise ValueError(f"验证 JSON 过大，请选择较小的回执目录: {relative}")
            raw = path.read_bytes()
            try:
                data = json.loads(raw.decode("utf-8-sig"))
            except (ValueError, UnicodeError) as error:
                raise ValueError(f"验证 JSON 无效: {relative}") from error
            evidence.append({"file": relative, "original_sha256": bytes_hash(raw), "data": redact(data)})
    return evidence


def json_bytes(value) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def zip_sources(target: Path, root: Path, files: list[dict], source_manifest: bytes) -> dict[str, str]:
    expected = {}
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for item in files:
            relative = item["path"]
            name = f"{SOURCE_NAME}/{relative}"
            path = source_path(root, relative)
            info = zipfile.ZipInfo.from_file(path, name)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (int(item["mode"], 8) << 16) | 0x20
            with path.open("rb") as source, archive.open(info, "w") as destination:
                shutil.copyfileobj(source, destination, READ_SIZE)
            expected[name] = item["sha256"]
        name = f"{SOURCE_NAME}/SOURCE-MANIFEST.json"
        archive.writestr(name, source_manifest)
        expected[name] = bytes_hash(source_manifest)
    return expected


def zip_portable(target: Path, folder: Path) -> dict[str, str]:
    expected = {}
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in sorted(folder.iterdir()):
            if not path.is_file():
                raise ValueError("运行目录包含非预期子目录")
            name = f"{PORTABLE_NAME}/{path.name}"
            archive.write(path, name)
            expected[name] = sha256(path)
    return expected


def verify_zip(path: Path, expected: dict[str, str]) -> None:
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(expected) or set(names) != set(expected):
            raise ValueError(f"ZIP 条目不一致: {path.name}")
        for name in names:
            pure = PurePosixPath(name)
            if pure.is_absolute() or ".." in pure.parts or "\\" in name:
                raise ValueError(f"ZIP 路径无效: {name}")
            digest = hashlib.sha256()
            with archive.open(name) as stream:
                for block in iter(lambda: stream.read(READ_SIZE), b""):
                    digest.update(block)
            if digest.hexdigest() != expected[name]:
                raise ValueError(f"ZIP 内容校验失败: {name}")


def write_checksums(folder: Path, paths: list[Path]) -> None:
    text = "".join(f"{sha256(path)}  {path.relative_to(folder).as_posix()}\n" for path in sorted(paths))
    (folder / "SHA256SUMS.txt").write_text(text, encoding="utf-8", newline="\n")


def parse_args(root: Path) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="打包已验收的原生划词翻译版本及对应源码。")
    parser.add_argument("--upstream", type=Path, default=root.parent / "sumatrapdf-translate", help="固定基线的只读 Git 检出")
    parser.add_argument("--release-exe", type=Path, default=root / "out/rel64/SumatraPDF-static.exe", help="已验证的 x64 EXE")
    parser.add_argument("--validation-dir", type=Path, action="append", default=[], help="out 下的 JSON 回执目录，可重复")
    parser.add_argument("--output-dir", type=Path, help="out/delivery 下尚不存在的批次目录")
    parser.add_argument("--inventory", action="store_true", help="只检查源码清单和补丁，不写交付文件")
    return parser.parse_args()


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    args = parse_args(root)
    delivery = within(root / "out/delivery", root)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    output = within(args.output_dir or delivery / stamp, delivery)
    if output == delivery:
        raise ValueError("请使用 out/delivery 下的独立批次目录")
    git = shutil.which("git")
    if not git:
        raise ValueError("未找到 Git")
    upstream = args.upstream.resolve()
    tracked = tracked_files(git, upstream)
    files, changed = inventory(root, upstream, tracked)
    patch = make_patch(root, upstream, git, tracked, changed)
    source_data = {"baseline": BASELINE, "upstream": UPSTREAM_URL, "files": files, "changed_files": changed}
    source_bytes = json_bytes(source_data)
    summary = {"source_files": len(files), "source_bytes": sum(item["bytes"] for item in files),
               "changed_files": changed, "patch_bytes": len(patch), "patch_check": "passed"}
    if args.inventory:
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        return 0
    exe = within(args.release_exe, root)
    if not exe.is_file():
        raise ValueError(f"发布 EXE 不存在: {exe}")
    pe = pe_info(exe)
    exe_hash = sha256(exe)
    evidence = read_evidence(root, args.validation_dir)
    manifest = {"baseline": BASELINE, "upstream": UPSTREAM_URL,
                "created_utc": datetime.now(timezone.utc).isoformat(), "real_model_verified": False,
                "executable": {"name": "SumatraPDF.exe", "input": exe.relative_to(root).as_posix(),
                               "bytes": exe.stat().st_size, "sha256": exe_hash, **pe},
                "source_manifest_sha256": bytes_hash(source_bytes), "patch_sha256": bytes_hash(patch),
                "verification_evidence": evidence}
    output.mkdir(parents=True, exist_ok=False)
    portable = output / PORTABLE_NAME
    portable.mkdir()
    shutil.copyfile(exe, portable / "SumatraPDF.exe")
    for name in ("COPYING", "COPYING.BSD", "AUTHORS"):
        shutil.copyfile(source_path(root, name), portable / name)
    shutil.copyfile(source_path(root, "docs/selection-translation.md"), portable / "README.md")
    (portable / "SumatraPDF-settings.txt").write_text(SETTINGS, encoding="utf-8", newline="\n")
    manifest_bytes = json_bytes(manifest)
    (portable / "MANIFEST.json").write_bytes(manifest_bytes)
    write_checksums(portable, [path for path in portable.iterdir() if path.is_file()])
    patch_path = output / "source.patch"
    patch_path.write_bytes(patch)
    (output / "SOURCE-MANIFEST.json").write_bytes(source_bytes)
    (output / "MANIFEST.json").write_bytes(manifest_bytes)
    product_zip = output / f"{PORTABLE_NAME}.zip"
    source_zip = output / f"{SOURCE_NAME}.zip"
    product_entries = zip_portable(product_zip, portable)
    source_entries = zip_sources(source_zip, root, files, source_bytes)
    verify_zip(product_zip, product_entries)
    verify_zip(source_zip, source_entries)
    if sha256(portable / "SumatraPDF.exe") != exe_hash:
        raise ValueError("复制后的 EXE 与发布文件不一致")
    result = {**summary, "output_directory": str(output), "portable_directory": str(portable),
              "executable_sha256": exe_hash, "portable_zip": {"file": product_zip.name,
              "bytes": product_zip.stat().st_size, "sha256": sha256(product_zip)},
              "source_zip": {"file": source_zip.name, "bytes": source_zip.stat().st_size,
              "sha256": sha256(source_zip)}, "zip_entry_count": {"portable": len(product_entries),
              "source": len(source_entries)}, "zip_crc_and_sha256": "passed", "real_model_verified": False}
    (output / "PACKAGE-RESULT.json").write_bytes(json_bytes(result))
    write_checksums(output, [path for path in output.iterdir() if path.is_file()])
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        print(f"打包失败: {error}", file=sys.stderr)
        raise SystemExit(1)
