package com.cogent.digimonworld;

import android.content.Context;
import android.content.res.AssetManager;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * Installs the support files the runtime reads from its data directory
 * (game.toml, BIOS, mods, the headers the translated game is compiled
 * against, ...). They ship as APK assets under payload/, listed with their
 * sizes in payload/manifest.txt, whose first line is "version <id>".
 * Everything is refreshed when that version changes; saves, the disc image
 * and the translation are never part of the payload, so they are untouched.
 */
final class PayloadInstaller {
    private PayloadInstaller() {}

    static void sync(Context context, File dataDir) throws IOException {
        AssetManager assets = context.getAssets();
        String manifest = readAll(assets.open("payload/manifest.txt"));
        String version = "";
        File stamp = new File(dataDir, ".payload_version");
        String installed = stamp.isFile() ? readAll(new FileInputStream(stamp)).trim() : "";

        String[] lines = manifest.split("\n");
        for (String line : lines) {
            if (line.startsWith("version ")) version = line.substring(8).trim();
        }
        boolean upgrade = !version.equals(installed);
        for (String line : lines) {
            int tab = line.indexOf('\t');
            if (line.startsWith("version ") || tab < 0) continue;
            long size = Long.parseLong(line.substring(0, tab));
            String rel = line.substring(tab + 1).trim();
            File dest = new File(dataDir, rel);
            if (!upgrade && dest.length() == size) continue;
            if (isPlayerOwned(rel, dest)) continue;
            copy(assets.open("payload/" + rel, AssetManager.ACCESS_STREAMING), dest);
        }
        if (upgrade) {
            try (OutputStream out = new FileOutputStream(stamp)) {
                out.write(version.getBytes(StandardCharsets.UTF_8));
            }
        }
    }

    /** Files the payload only seeds: once the player has made choices (the
     *  in-game menu writes mods/state.toml), updates must not reset them. The
     *  empty state file older versions installed still counts as a default. */
    static boolean isPlayerOwned(String rel, File dest) throws IOException {
        if (!rel.equals("mods/state.toml") || !dest.isFile()) return false;
        return readAll(new FileInputStream(dest)).contains("[[");
    }

    /** Copies through a temp file so an interrupted copy never looks complete. */
    static void copy(InputStream in, File dest) throws IOException {
        File parent = dest.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs())
            throw new IOException("cannot create " + parent);
        File tmp = new File(dest.getPath() + ".part");
        try (InputStream src = in; OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = src.read(buf)) > 0) out.write(buf, 0, n);
        }
        if (!tmp.renameTo(dest)) {
            tmp.delete();
            throw new IOException("cannot write " + dest);
        }
    }

    static String readAll(InputStream in) throws IOException {
        try (InputStream src = in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = src.read(buf)) > 0) out.write(buf, 0, n);
            return out.toString("UTF-8");
        }
    }
}
