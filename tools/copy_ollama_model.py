#!/usr/bin/env python3
"""Copy one already-installed Ollama model into R2's private, r2-owned model store.

This does not download anything. It copies the manifest and its content-addressed
blobs from the existing local Ollama store so the dedicated r2-owned server can
serve the same model without sharing the user's inference process.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import stat
import subprocess
import sys
import tempfile


def service_model_root(model: str) -> Path | None:
    env_text = ""
    service_user = "ollama"
    try:
        env_text = subprocess.run(
            ["systemctl", "show", "ollama.service", "-p", "Environment", "--value"],
            check=False, capture_output=True, text=True, timeout=5,
        ).stdout.strip()
        user_text = subprocess.run(
            ["systemctl", "show", "ollama.service", "-p", "User", "--value"],
            check=False, capture_output=True, text=True, timeout=5,
        ).stdout.strip()
        if user_text:
            service_user = user_text
    except (OSError, subprocess.SubprocessError):
        pass
    for item in env_text.split():
        if item.startswith("OLLAMA_MODELS="):
            value = item.split("=", 1)[1].strip().strip('"')
            if value:
                configured = Path(value)
                try:
                    if (configured / model_manifest_relative(model)).is_file():
                        return configured
                except ValueError:
                    pass
    try:
        home = Path(pwd.getpwnam(service_user).pw_dir)
    except KeyError:
        home = Path("/usr/share/ollama")
    candidates = [
        home / ".ollama" / "models",
        Path("/usr/share/ollama/.ollama/models"),
        Path("/var/lib/ollama/.ollama/models"),
        Path("/home/x/.ollama/models"),
    ]
    return next((p for p in candidates if p.is_dir() and (p / model_manifest_relative(model)).is_file()), None)


def model_manifest_relative(model: str) -> Path:
    """Map a model reference to a manifest path without accepting path syntax."""
    if not isinstance(model, str) or not model or model.count(":") > 1:
        raise ValueError(f"invalid Ollama model reference: {model!r}")
    if ":" in model:
        name, tag = model.rsplit(":", 1)
    else:
        name, tag = model, "latest"

    # Model names may be unqualified or namespace/model; tags are a single
    # component. Reject separators, traversal, empty components, and other
    # path syntax before constructing a path used by a privileged installer.
    parts = name.split("/")
    valid_component = lambda value: (
        bool(value)
        and value not in {".", ".."}
        and all(char.isalnum() or char in "._-" for char in value)
    )
    if (not parts or any(not valid_component(part) for part in parts)
            or not valid_component(tag)):
        raise ValueError(f"invalid Ollama model reference: {model!r}")
    if len(parts) == 1:
        parts = ["library", parts[0]]
    return Path("manifests") / "registry.ollama.ai" / Path(*parts) / tag


def _reject_symlink_path_components(path: Path) -> None:
    """Refuse symlinked parent components before a privileged copy writes beneath them."""
    absolute = path.absolute()
    current = Path(absolute.anchor)
    for component in absolute.parts[1:]:
        current = current / component
        try:
            mode = current.lstat().st_mode
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(mode):
            raise ValueError(f"refusing symlinked destination path component: {current}")


def _sha256_file(path: Path) -> str:
    """Hash a model blob without loading its potentially multi-gigabyte bytes into RAM."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy_model(source_root: Path, destination_root: Path, model: str, owner: str) -> None:
    rel = model_manifest_relative(model)
    source_manifest = source_root / rel
    if not source_manifest.is_file():
        raise FileNotFoundError(
            f"{model} is not installed in {source_root}. Start the existing Ollama service, "
            f"pull {model} there, and rerun this installer; this tool never downloads models."
        )
    manifest = json.loads(source_manifest.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict):
        raise ValueError(f"invalid Ollama manifest object: {source_manifest}")
    config = manifest.get("config")
    layers = manifest.get("layers")
    if not isinstance(config, dict) or not isinstance(config.get("digest"), str):
        raise ValueError(f"manifest config is missing a blob digest: {source_manifest}")
    if not isinstance(layers, list) or not layers:
        raise ValueError(f"manifest layers must be a non-empty list: {source_manifest}")

    # A malformed manifest must not be partially accepted. Skipping a layer
    # with a missing or non-string digest would produce a copied manifest whose
    # referenced blob was never copied, leaving the private model unusable.
    digests = [config["digest"]]
    for index, layer in enumerate(layers):
        if not isinstance(layer, dict) or not isinstance(layer.get("digest"), str):
            raise ValueError(
                f"manifest layer {index} is missing a blob digest: {source_manifest}"
            )
        digests.append(layer["digest"])

    # Validate every digest before touching the destination. Digests become
    # filenames below, so length/prefix checks alone are insufficient: a
    # malformed manifest could put separators and parent components in a digest.
    for digest in digests:
        if (not digest.startswith("sha256:") or len(digest) != 71
                or any(char not in "0123456789abcdef" for char in digest[7:])):
            raise ValueError(f"unsupported blob digest in manifest: {digest}")

    uid = gid = None
    if owner:
        account = pwd.getpwnam(owner)
        uid, gid = account.pw_uid, account.pw_gid

    # The installer invokes this copier with elevated privileges, while the
    # destination tree is writable by the r2 runtime account. Do not let a
    # pre-existing symlink redirect root-owned writes outside the model store.
    _reject_symlink_path_components(destination_root)
    _reject_symlink_path_components(destination_root / "blobs")
    _reject_symlink_path_components(destination_root / rel.parent)
    (destination_root / "blobs").mkdir(parents=True, exist_ok=True)
    (destination_root / rel.parent).mkdir(parents=True, exist_ok=True)
    for digest in sorted(set(digests)):
        expected_sha256 = digest[7:]
        filename = "sha256-" + expected_sha256
        src = source_root / "blobs" / filename
        dst = destination_root / "blobs" / filename
        _reject_symlink_path_components(dst.parent)
        if not src.is_file():
            raise FileNotFoundError(f"model blob is missing: {src}")
        # Ollama blob names are content hashes. Size equality alone is not proof
        # that a pre-existing destination is intact; verify both source and
        # destination so a corrupt same-size blob cannot survive an upgrade.
        if _sha256_file(src) != expected_sha256:
            raise ValueError(f"source model blob failed SHA-256 verification: {src}")
        if dst.is_symlink() or not dst.is_file() or _sha256_file(dst) != expected_sha256:
            # Create the temporary file exclusively in the destination directory.
            # A predictable PID-based name could already be a symlink and redirect
            # shutil.copyfile() outside the model store.
            fd, tmp_name = tempfile.mkstemp(prefix=dst.name + ".tmp.", dir=dst.parent)
            tmp = Path(tmp_name)
            try:
                with os.fdopen(fd, "wb") as output, src.open("rb") as input_stream:
                    shutil.copyfileobj(input_stream, output, length=1024 * 1024)
                    output.flush()
                    os.fsync(output.fileno())
                if _sha256_file(tmp) != expected_sha256:
                    raise ValueError(f"copied model blob failed SHA-256 verification: {tmp}")
                os.chmod(tmp, 0o644)
                os.replace(tmp, dst)
            finally:
                try:
                    tmp.unlink()
                except FileNotFoundError:
                    pass
        if uid is not None:
            os.chown(dst, uid, gid)
        os.chmod(dst, 0o644)

    target_manifest = destination_root / rel
    _reject_symlink_path_components(target_manifest.parent)
    manifest_fd, manifest_tmp_name = tempfile.mkstemp(
        prefix=target_manifest.name + ".tmp.", dir=target_manifest.parent
    )
    tmp_manifest = Path(manifest_tmp_name)
    try:
        with os.fdopen(manifest_fd, "wb") as output, source_manifest.open("rb") as input_stream:
            shutil.copyfileobj(input_stream, output, length=1024 * 1024)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(tmp_manifest, 0o644)
        os.replace(tmp_manifest, target_manifest)
    finally:
        try:
            tmp_manifest.unlink()
        except FileNotFoundError:
            pass
    if uid is not None:
        os.chown(target_manifest, uid, gid)
    manifest_parents = []
    parent = target_manifest.parent
    while parent == destination_root or destination_root in parent.parents:
        manifest_parents.append(parent)
        if parent == destination_root:
            break
        parent = parent.parent
    for directory in [destination_root, destination_root / "blobs", *manifest_parents]:
        if uid is not None:
            os.chown(directory, uid, gid)
        os.chmod(directory, 0o755)
    print(f"Copied local Ollama model {model} to {destination_root} for user {owner or os.getuid()}.")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default=os.environ.get("R2_MODEL", "gemma3:4b"))
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--destination-root", type=Path, default=Path("/home/x/R2_Home/.ollama/models"))
    parser.add_argument("--owner", default="r2")
    args = parser.parse_args()
    try:
        # Validate before probing the host's model-store paths, so malformed
        # references fail deterministically even when no candidate store exists.
        model_manifest_relative(args.model)
        source = args.source_root or service_model_root(args.model)
        if source is None or not source.is_dir():
            print("ERROR: could not locate the existing local Ollama model store. Use --source-root.", file=sys.stderr)
            return 1
        copy_model(source, args.destination_root, args.model, args.owner)
    except (OSError, ValueError, json.JSONDecodeError, KeyError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1    return 0


if __name__ == "__main__":
    raise SystemExit(main())