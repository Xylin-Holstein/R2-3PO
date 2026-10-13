#!/usr/bin/env python3
"""Copy one already-installed Ollama model into R2's private, r2-owned model store.

This does not download anything. It copies the manifest and its content-addressed
blobs from the existing local Ollama store so the dedicated r2-owned server can
serve the same model without sharing the user's inference process.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import sys


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
    if ":" in model:
        name, tag = model.rsplit(":", 1)
    else:
        name, tag = model, "latest"
    if not name or not tag or name.startswith("/") or ".." in Path(name).parts:
        raise ValueError(f"invalid Ollama model reference: {model}")
    parts = name.split("/")
    if len(parts) == 1:
        parts = ["library", parts[0]]
    return Path("manifests") / "registry.ollama.ai" / Path(*parts) / tag


def copy_model(source_root: Path, destination_root: Path, model: str, owner: str) -> None:
    rel = model_manifest_relative(model)
    source_manifest = source_root / rel
    if not source_manifest.is_file():
        raise FileNotFoundError(
            f"{model} is not installed in {source_root}. Start the existing Ollama service, "
            f"pull {model} there, and rerun this installer; this tool never downloads models."
        )
    manifest = json.loads(source_manifest.read_text(encoding="utf-8"))
    digests = []
    config = manifest.get("config", {})
    if isinstance(config, dict) and isinstance(config.get("digest"), str):
        digests.append(config["digest"])
    for layer in manifest.get("layers", []):
        if isinstance(layer, dict) and isinstance(layer.get("digest"), str):
            digests.append(layer["digest"])
    if not digests:
        raise ValueError(f"manifest contains no model blobs: {source_manifest}")

    uid = gid = None
    if owner:
        account = pwd.getpwnam(owner)
        uid, gid = account.pw_uid, account.pw_gid

    (destination_root / "blobs").mkdir(parents=True, exist_ok=True)
    (destination_root / rel.parent).mkdir(parents=True, exist_ok=True)
    for digest in sorted(set(digests)):
        if not digest.startswith("sha256:") or len(digest) != 71:
            raise ValueError(f"unsupported blob digest in manifest: {digest}")
        filename = "sha256-" + digest.split(":", 1)[1]
        src = source_root / "blobs" / filename
        dst = destination_root / "blobs" / filename
        if not src.is_file():
            raise FileNotFoundError(f"model blob is missing: {src}")
        if not dst.is_file() or dst.stat().st_size != src.stat().st_size:
            tmp = dst.with_name(dst.name + f".tmp.{os.getpid()}")
            shutil.copyfile(src, tmp)
            os.chmod(tmp, 0o644)
            os.replace(tmp, dst)
        if uid is not None:
            os.chown(dst, uid, gid)
        os.chmod(dst, 0o644)

    target_manifest = destination_root / rel
    tmp_manifest = target_manifest.with_name(target_manifest.name + f".tmp.{os.getpid()}")
    shutil.copyfile(source_manifest, tmp_manifest)
    os.chmod(tmp_manifest, 0o644)
    os.replace(tmp_manifest, target_manifest)
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
    source = args.source_root or service_model_root(args.model)
    if source is None or not source.is_dir():
        print("ERROR: could not locate the existing local Ollama model store. Use --source-root.", file=sys.stderr)
        return 1
    try:
        copy_model(source, args.destination_root, args.model, args.owner)
    except (OSError, ValueError, json.JSONDecodeError, KeyError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
