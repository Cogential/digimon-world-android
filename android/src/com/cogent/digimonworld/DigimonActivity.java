package com.cogent.digimonworld;

import android.content.Intent;
import android.os.Bundle;
import android.view.Display;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;

/**
 * The game is SDL's native activity; this only sets up the window around it.
 * SetupActivity has already installed the support files, imported the disc
 * and translated the game; libmain.so compiles that translation and runs it.
 */
public class DigimonActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected String[] getArguments() {
        String disc = getIntent().getStringExtra(SetupActivity.EXTRA_DISC);
        if (disc == null) {
            File d = SetupActivity.currentDisc(SetupActivity.dataDir(this));
            if (d != null) disc = d.getPath();
        }
        if (disc == null) return new String[] { "--renderer", "software" };
        return new String[] { "--renderer", "software", "--disc", disc };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (!SetupActivity.isReady(SetupActivity.dataDir(this))) {
            // Restored from recents without a disc or translation (e.g. after
            // "Change disc"): go through setup instead of starting a game.
            super.onCreate(savedInstanceState);
            startActivity(new Intent(this, SetupActivity.class));
            finish();
            return;
        }
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        requestSixtyHz();
    }

    /** Called by libmain.so (in-game menu): relaunch through the setup screen,
     *  so a changed disc or a setting that applies at boot takes effect. */
    public void restartApp() {
        runOnUiThread(new Runnable() {
            public void run() {
                Intent intent = new Intent(DigimonActivity.this, SetupActivity.class);
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                startActivity(intent);
                android.os.Process.killProcess(android.os.Process.myPid());
            }
        });
    }

    /** Called by libmain.so (in-game menu). */
    public void quitApp() {
        runOnUiThread(new Runnable() {
            public void run() {
                finishAndRemoveTask();
                android.os.Process.killProcess(android.os.Process.myPid());
            }
        });
    }

    /* The game runs at 60 frames a second. On a 120 Hz panel ask for a 60 Hz
     * mode at the current resolution so every game frame is shown for exactly
     * one refresh. */
    private void requestSixtyHz() {
        Display display = getDisplay();
        if (display == null) return;
        Display.Mode current = display.getMode();
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        for (Display.Mode m : display.getSupportedModes()) {
            if (m.getPhysicalWidth() == current.getPhysicalWidth()
                    && m.getPhysicalHeight() == current.getPhysicalHeight()
                    && Math.abs(m.getRefreshRate() - 60.0f) < 1.0f) {
                lp.preferredDisplayModeId = m.getModeId();
                break;
            }
        }
        lp.preferredRefreshRate = 60.0f;
        getWindow().setAttributes(lp);
    }
}
