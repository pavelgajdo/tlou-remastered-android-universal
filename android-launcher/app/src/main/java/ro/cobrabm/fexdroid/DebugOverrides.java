package ro.cobrabm.fexdroid;

import android.content.Context;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Map;

/**
 * Debugging through the app's external files folder (/sdcard/Android/data/&lt;app id&gt;/files),
 * which adb can write without root:
 * env.txt: KEY=VALUE lines (# comments), added to the runtime environment last;
 * toggles.txt: the runtime's disabled-optimizations mask, re-read while the game runs;
 * TLOU_EXTERNAL_LOG=1 in env.txt writes the runtime log to runtime.log there instead.
 */
final class DebugOverrides {
    private DebugOverrides() {}

    static void apply(Context context, Map<String, String> env) {
        File dir = context.getExternalFilesDir(null);
        if (dir == null) return;
        env.put("GOW3_TOGGLE_FILE", new File(dir, "toggles.txt").getAbsolutePath());
        File file = new File(dir, "env.txt");
        if (!file.isFile()) return;
        StringBuilder applied = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            for (String line; (line = reader.readLine()) != null; ) {
                line = line.trim();
                int eq = line.indexOf('=');
                if (line.isEmpty() || line.startsWith("#") || eq <= 0) continue;
                env.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
                applied.append(' ').append(line);
            }
        } catch (IOException e) {
            RunLog.write(context, "env.txt unreadable: " + e);
            return;
        }
        RunLog.write(context, "env.txt:" + applied);
    }

    /**
     * Native libraries in override/ (for example a locally built libgow3gpu.so) are copied into
     * app storage, where they may be loaded, and searched before the APK's. Without the folder
     * the copies are removed and the APK's libraries load again.
     */
    static void overrideLibraries(Context context, Map<String, String> env) {
        File dir = context.getExternalFilesDir(null);
        File target = new File(context.getFilesDir(), "override");
        File[] old = target.listFiles();
        if (old != null) for (File f : old) f.delete();
        File[] libs = dir == null ? null
                : new File(dir, "override").listFiles((d, name) -> name.endsWith(".so"));
        if (libs == null || libs.length == 0) return;
        StringBuilder names = new StringBuilder();
        try {
            Files.createDirectories(target.toPath());
            for (File lib : libs) {
                Files.copy(lib.toPath(), new File(target, lib.getName()).toPath(),
                        StandardCopyOption.REPLACE_EXISTING);
                names.append(' ').append(lib.getName()).append(" (").append(lib.length()).append(" bytes)");
            }
        } catch (IOException e) {
            RunLog.write(context, "override libraries not copied: " + e);
            return;
        }
        String path = env.get("LD_LIBRARY_PATH");
        env.put("LD_LIBRARY_PATH", target.getAbsolutePath() + (path == null ? "" : ":" + path));
        RunLog.write(context, "override libraries:" + names);
    }

    static File runtimeLog(Context context, Map<String, String> env, File normal) {
        File dir = context.getExternalFilesDir(null);
        if (dir == null || !"1".equals(env.get("TLOU_EXTERNAL_LOG"))) return normal;
        return new File(dir, "runtime.log");
    }
}
