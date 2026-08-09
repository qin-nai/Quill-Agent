package com.hermes.agent;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.webkit.JavascriptInterface;

import java.io.File;

/**
 * WebView 暴露给 JS 的桥:长按文件 → "用其他应用打开"。
 * Intent 必须从主线程启动,故用 Handler.post 切回主线程。
 */
public class FileBridge {
    private final Activity activity;
    private final File workspace;

    public FileBridge(Activity activity, File workspace) {
        this.activity = activity;
        this.workspace = workspace;
    }

    /** JS 调用:AndroidBridge.openWithApp(workspace 相对路径) */
    @JavascriptInterface
    public void openWithApp(final String relPath) {
        if (relPath == null || !isInsideWorkspace(relPath)) return;
        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                Uri.Builder b = new Uri.Builder()
                        .scheme("content")
                        .authority(WorkspaceFileProvider.AUTHORITY);
                for (String seg : relPath.split("/")) {
                    if (!seg.isEmpty()) b.appendPath(seg);
                }
                Uri uri = b.build();
                String mime = activity.getContentResolver().getType(uri);
                Intent intent = new Intent(Intent.ACTION_VIEW);
                intent.setDataAndType(uri, mime != null ? mime : "*/*");
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                activity.startActivity(Intent.createChooser(intent, "用其他应用打开"));
            } catch (Exception e) {
                // 没有能打开该类型文件的应用 → 静默,不打断用户
            }
        });
    }

    private boolean isInsideWorkspace(String rel) {
        if (rel.startsWith("/") || rel.startsWith("\\") || rel.contains("..")) return false;
        File f = new File(workspace, rel);
        try {
            String baseCanon = workspace.getCanonicalPath();
            String fileCanon = f.getCanonicalPath();
            return f.isFile() && fileCanon.startsWith(baseCanon + File.separator);
        } catch (Exception e) {
            return false;
        }
    }
}
