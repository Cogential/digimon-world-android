package com.cogent.digimonworld;

import android.app.Activity;
import android.content.ClipData;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * First screen of the app. The APK contains nothing from the game, so before
 * the game can start this screen:
 *  1. installs the runtime's support files (PayloadInstaller),
 *  2. asks for the player's own disc image and copies it into app storage,
 *  3. translates the game's program into C once (nativeTranslate: the
 *     recompiler runs on the phone), which the game screen compiles and runs.
 * On later launches it only checks that all of that is in place and goes
 * straight to the game.
 */
public class SetupActivity extends Activity {

    /** SHA-256 of SLUS_010.32, the program on Digimon World (USA) that this
     *  port's configuration, patches and mods are written against. */
    static final String EXE_SHA256 =
            "341817d3b2091c4c3e83c74ec71157ef6ccf664f27edf855406d8a8dfbadbe66";
    static final String DISC_BASE = "Digimon World (USA)";
    static final String EXTRA_DISC = "disc";
    private static final int REQUEST_DISC = 1;

    static {
        System.loadLibrary("SDL3");
        System.loadLibrary("main");
    }

    static native String nativeTranslate(String dataDir, String discPath, String exeSha256);
    static native boolean nativeIsTranslated(String dataDir, String exeSha256);

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private File dataDir;
    private TextView message;
    private TextView status;
    private ProgressBar progress;
    private Button pickButton;

    /** Where the runtime keeps everything (same directory SDL reports as its
     *  external storage path): reachable over USB, no permission needed. */
    static File dataDir(Activity a) {
        File d = a.getExternalFilesDir(null);
        return d != null ? d : a.getFilesDir();
    }

    /** The imported disc image (a .cue next to its .bin, or a .chd), or null. */
    static File currentDisc(File dataDir) {
        File disc = new File(dataDir, "disc");
        File chd = new File(disc, DISC_BASE + ".chd");
        if (chd.isFile()) return chd;
        File cue = new File(disc, DISC_BASE + ".cue");
        File bin = new File(disc, DISC_BASE + ".bin");
        if (cue.isFile() && bin.isFile()) return cue;
        return null;
    }

    /** Disc imported and translated: the game screen can start. */
    static boolean isReady(File dataDir) {
        return currentDisc(dataDir) != null
                && new File(dataDir, "generated/.translated").isFile();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        dataDir = dataDir(this);
        buildUi();
        showBusy("Getting ready…", -1);
        worker.execute(new Runnable() { public void run() { prepare(); } });
    }

    @Override
    protected void onDestroy() {
        worker.shutdownNow();
        super.onDestroy();
    }

    /* ---- flow ------------------------------------------------------------ */

    private void prepare() {
        try {
            PayloadInstaller.sync(this, dataDir);
        } catch (IOException e) {
            final String msg = "Could not install the game's support files: " + e.getMessage();
            ui.post(new Runnable() { public void run() { showError(msg, false); } });
            return;
        }
        final File disc = currentDisc(dataDir);
        if (disc == null) {
            ui.post(new Runnable() { public void run() { showWelcome(); } });
        } else if (nativeIsTranslated(dataDir.getPath(), EXE_SHA256)) {
            ui.post(new Runnable() { public void run() { startGame(disc); } });
        } else {
            translate(disc);
        }
    }

    private void translate(final File disc) {
        ui.post(new Runnable() { public void run() {
            showBusy("Translating the game for your phone…\n"
                    + "This happens once and takes a few seconds.", -1);
        } });
        final String err = nativeTranslate(dataDir.getPath(), disc.getPath(), EXE_SHA256);
        if (err == null) {
            ui.post(new Runnable() { public void run() { startGame(disc); } });
        } else {
            deleteDisc();
            ui.post(new Runnable() { public void run() { showError(err, true); } });
        }
    }

    private void startGame(File disc) {
        Intent intent = new Intent(this, DigimonActivity.class);
        intent.putExtra(EXTRA_DISC, disc.getPath());
        startActivity(intent);
        finish();
    }

    /* ---- disc import ------------------------------------------------------- */

    private void pickDisc() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(intent, REQUEST_DISC);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_DISC || resultCode != RESULT_OK || data == null) return;
        List<Uri> uris = new ArrayList<>();
        ClipData clip = data.getClipData();
        if (clip != null) {
            for (int i = 0; i < clip.getItemCount(); i++) uris.add(clip.getItemAt(i).getUri());
        } else if (data.getData() != null) {
            uris.add(data.getData());
        }
        Uri chosen = null;
        String chosenName = null;
        long chosenSize = -1;
        boolean sawCue = false, sawIso = false;
        for (Uri uri : uris) {
            String name = displayName(uri).toLowerCase(Locale.ROOT);
            long size = fileSize(uri);
            if (name.endsWith(".cue")) { sawCue = true; continue; }
            if (name.endsWith(".iso")) { sawIso = true; continue; }
            boolean chd = name.endsWith(".chd");
            // Prefer a .chd, then the largest other file (the .bin).
            if (chosen == null || chd || (!chosenName.endsWith(".chd") && size > chosenSize)) {
                chosen = uri;
                chosenName = name;
                chosenSize = size;
            }
        }
        if (chosen == null) {
            showError(sawIso
                    ? "An .iso image leaves out data PlayStation discs need. Use a .bin/.cue or .chd dump."
                    : sawCue
                    ? "That's the small .cue file. Choose the .bin file next to it (the big one), or a .chd."
                    : "No disc image was chosen.", true);
            return;
        }
        final Uri uri = chosen;
        final boolean chd = chosenName.endsWith(".chd");
        final long size = chosenSize;
        worker.execute(new Runnable() { public void run() { importDisc(uri, chd, size); } });
    }

    private void importDisc(Uri uri, boolean chd, long size) {
        deleteDisc();
        File discDir = new File(dataDir, "disc");
        File dest = new File(discDir, DISC_BASE + (chd ? ".chd" : ".bin"));
        File tmp = new File(dest.getPath() + ".part");
        try {
            if (!discDir.isDirectory() && !discDir.mkdirs())
                throw new IOException("cannot create " + discDir);
            try (InputStream in = getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(tmp)) {
                if (in == null) throw new IOException("the file could not be opened");
                byte[] buf = new byte[1 << 20];
                long done = 0;
                int lastPct = -1, n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    done += n;
                    int pct = size > 0 ? (int) (done * 100 / size) : -1;
                    if (pct != lastPct) {
                        lastPct = pct;
                        final int p = pct;
                        ui.post(new Runnable() { public void run() {
                            showBusy("Copying your disc image…", p);
                        } });
                    }
                }
            }
            if (!tmp.renameTo(dest)) throw new IOException("cannot write " + dest);
            if (!chd) {
                String cue = "FILE \"" + DISC_BASE + ".bin\" BINARY\n"
                        + "  TRACK 01 MODE2/2352\n"
                        + "    INDEX 01 00:00:00\n";
                try (OutputStream out = new FileOutputStream(new File(discDir, DISC_BASE + ".cue"))) {
                    out.write(cue.getBytes("UTF-8"));
                }
            }
        } catch (IOException e) {
            tmp.delete();
            deleteDisc();
            final String msg = "Copying the disc image failed: " + e.getMessage()
                    + "\nCheck there is at least 500 MB of free space.";
            ui.post(new Runnable() { public void run() { showError(msg, true); } });
            return;
        }
        translate(currentDisc(dataDir));
    }

    private void deleteDisc() {
        File discDir = new File(dataDir, "disc");
        for (String ext : new String[] { ".bin", ".cue", ".chd", ".bin.part", ".chd.part" }) {
            new File(discDir, DISC_BASE + ext).delete();
        }
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri,
                new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (RuntimeException ignored) {
        }
        String last = uri.getLastPathSegment();
        return last != null ? last : "";
    }

    private long fileSize(Uri uri) {
        try (Cursor c = getContentResolver().query(uri,
                new String[] { OpenableColumns.SIZE }, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (RuntimeException ignored) {
        }
        return -1;
    }

    /* ---- UI ---------------------------------------------------------------- */

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                getResources().getDisplayMetrics());
    }

    private void buildUi() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        col.setPadding(dp(28), dp(40), dp(28), dp(28));

        ImageView icon = new ImageView(this);
        icon.setImageResource(R.mipmap.ic_launcher);
        col.addView(icon, new LinearLayout.LayoutParams(dp(96), dp(96)));

        TextView title = new TextView(this);
        title.setText("Digimon World");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, dp(16), 0, dp(12));
        col.addView(title);

        message = new TextView(this);
        message.setTextColor(0xFFCCCCCC);
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        message.setGravity(Gravity.CENTER);
        message.setLineSpacing(0, 1.15f);
        col.addView(message);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100);
        LinearLayout.LayoutParams pl = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        pl.topMargin = dp(24);
        col.addView(progress, pl);

        status = new TextView(this);
        status.setTextColor(0xFF9FB6FF);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(8), 0, 0);
        col.addView(status);

        pickButton = new Button(this);
        pickButton.setText("Choose disc image");
        pickButton.setAllCaps(false);
        pickButton.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        pickButton.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { pickDisc(); }
        });
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        bl.topMargin = dp(24);
        col.addView(pickButton, bl);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(0xFF0B0E14);
        scroll.setFillViewport(true);
        scroll.addView(col);
        setContentView(scroll);
    }

    private void showWelcome() {
        message.setText("This app contains nothing from the game. To play, choose your own "
                + "copy of Digimon World (USA, SLUS-01032) as a disc image: the .bin file "
                + "of a .bin/.cue dump, or a .chd.\n\n"
                + "It is copied into the app and translated for your phone once. "
                + "After that the game starts straight away.");
        status.setText("");
        progress.setVisibility(View.GONE);
        pickButton.setVisibility(View.VISIBLE);
    }

    private void showBusy(String text, int pct) {
        status.setText(text);
        status.setTextColor(0xFF9FB6FF);
        progress.setVisibility(View.VISIBLE);
        progress.setIndeterminate(pct < 0);
        if (pct >= 0) progress.setProgress(pct);
        pickButton.setVisibility(View.GONE);
    }

    private void showError(String text, boolean canRetry) {
        if (message.getText().length() == 0) showWelcome();
        status.setText(text);
        status.setTextColor(0xFFFF8A80);
        progress.setVisibility(View.GONE);
        pickButton.setVisibility(canRetry ? View.VISIBLE : View.GONE);
        pickButton.setText("Choose another file");
    }
}
