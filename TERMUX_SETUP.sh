#!/bin/bash
# ============================================================
# SETUP & PUSH - libantifeedback
# Jalankan di Termux, satu per satu atau paste semua.
# ============================================================

# ── LANGKAH 1: Buat repo baru di GitHub ─────────────────────
# Buka browser, buat repo baru:
#   https://github.com/new
#   Name: libantifeedback
#   Visibility: Private atau Public (terserah)
#   Jangan centang "Initialize this repository"
#   Klik "Create repository"
#
# ATAU pakai gh CLI (jika sudah login):
gh repo create brruham-arch/libantifeedback --private --confirm

# ── LANGKAH 2: Ekstrak zip ke Termux ────────────────────────
# Pindahkan zip dari download ke home Termux dulu:
cp /storage/emulated/0/Download/libantifeedback.zip ~/
cd ~
unzip libantifeedback.zip
cd libantifeedback

# ── LANGKAH 3: Init git + remote ────────────────────────────
git init
git config user.email "email@gmail.com"
git config user.name "brruham-arch"
git remote add origin https://github.com/brruham-arch/libantifeedback.git

# ── LANGKAH 4: Push ke GitHub ───────────────────────────────
git add .
git commit -m "init: libantifeedback v1.0 - native anti-feedback SV"
git branch -M main
git push -u origin main

# ── LANGKAH 5: Pantau build GitHub Actions ──────────────────
gh run watch $(gh run list --limit 1 --json databaseId -q '.[0].databaseId')

# ── LANGKAH 6: Download hasil build ─────────────────────────
gh run download \
  $(gh run list --limit 1 --json databaseId -q '.[0].databaseId') \
  -n libantifeedback-arm32 \
  -D ~/output/

ls -lh ~/output/

# ── LANGKAH 7: Copy ke mods folder ──────────────────────────
cp ~/output/libantifeedback.so \
  /storage/emulated/0/Android/data/com.sampmobilerp.game/mods/

# ── PERINTAH HARIAN (setelah edit kode) ─────────────────────
# cd ~/libantifeedback
# git add mod/main.cpp
# git commit -m "fix: deskripsi perubahan"
# git push
# gh run watch $(gh run list --limit 1 --json databaseId -q '.[0].databaseId')
# gh run download $(gh run list --limit 1 --json databaseId -q '.[0].databaseId') -n libantifeedback-arm32 -D ~/output/

# ── CEK LOG SETELAH INSTALL ──────────────────────────────────
# tail -f /storage/emulated/0/antifeedback_log.txt
