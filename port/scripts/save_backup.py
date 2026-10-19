#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Backups of the game's saves: <user>/savedata copied to <user>/savedata_backups/<time>.

run.py makes one before every start; the launcher lists them and restores one. A restore
first backs up the current saves, so it can always be undone.
"""
import datetime
from pathlib import Path
import re
import shutil

KEEP = 10
STAMP = '%Y%m%d-%H%M%S'
BACKUP_NAME = re.compile(r'([0-9]{8}-[0-9]{6})(?:-([0-9]+))?')


def _backup_order(name):
    """(timestamp, sequence) of a backup folder; other names keep their own order."""
    match = BACKUP_NAME.fullmatch(name)
    return (match[1], int(match[2] or 1)) if match else (name, 0)


def backups_dir(user_dir):
    return Path(user_dir) / 'savedata_backups'


def list_backups(user_dir):
    """Backup names, newest first (same-second backups order by their -N suffix)."""
    root = backups_dir(user_dir)
    return sorted((p.name for p in root.iterdir() if p.is_dir()), key=_backup_order,
                  reverse=True) if root.is_dir() else []


def _has_saves(savedata):
    return savedata.is_dir() and any(p.is_file() for p in savedata.rglob('*'))


def _prune(user_dir, keep=KEEP):
    for name in list_backups(user_dir)[keep:]:
        shutil.rmtree(backups_dir(user_dir) / name, ignore_errors=True)


def _copy(user_dir, now):
    savedata = Path(user_dir) / 'savedata'
    if not _has_saves(savedata):
        return None
    root = backups_dir(user_dir)
    root.mkdir(parents=True, exist_ok=True)
    base = (now or datetime.datetime.now()).strftime(STAMP)
    # Pruning may have removed the unsuffixed folder or left gaps: number the new one after the
    # highest already used, so it never sorts behind an older backup of the same second.
    n = 1
    for entry in root.iterdir():
        stamp, sequence = _backup_order(entry.name)
        if stamp == base:
            n = max(n, sequence + 1)
    target = root / (base if n == 1 else f'{base}-{n}')
    shutil.copytree(savedata, target)
    return target


def backup(user_dir, now=None, keep=KEEP):
    """Copies savedata; returns the new folder, or None when there is nothing to save."""
    made = _copy(user_dir, now)
    if made:
        _prune(user_dir, keep)
    return made


def restore(user_dir, name, now=None, keep=KEEP):
    """Replaces savedata with backup `name`; returns the backup of the saves it replaced."""
    if not name or name not in list_backups(user_dir):
        raise ValueError(f'No save backup named {name!r}')
    source = backups_dir(user_dir) / name
    savedata = Path(user_dir) / 'savedata'
    # Pruning waits until the copy is done: the restored backup may be the oldest one.
    safety = _copy(user_dir, now)
    staging = Path(user_dir) / 'savedata.restoring'
    shutil.rmtree(staging, ignore_errors=True)
    shutil.copytree(source, staging)
    # Swap by renames: the current saves stay on disk (as `savedata.replaced`) until the restored
    # ones are in place, so a failure never leaves the folder without saves.
    aside = Path(user_dir) / 'savedata.replaced'
    shutil.rmtree(aside, ignore_errors=True)
    if savedata.exists():
        savedata.rename(aside)
    try:
        staging.rename(savedata)
    except OSError:
        if aside.exists():
            aside.rename(savedata)
        raise
    shutil.rmtree(aside, ignore_errors=True)
    _prune(user_dir, keep)
    return safety

