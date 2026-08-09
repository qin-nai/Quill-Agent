package com.hermes.agent;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.WebView;
import android.webkit.WebSettings;
import android.webkit.WebViewClient;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public class MainActivity extends Activity {
    static { System.loadLibrary("hermes_native"); }

    public static native void initNative();
    public static native void startServer(String webui, String workspace, String data, int port);

    private WebView webView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        initNative();  // 主线程缓存 HermesHttp 类引用(JNI),否则 worker 线程 FindClass 会失败

        // 把 assets/webui 解压到私有目录(server 用文件系统路径托管静态资源)
        File files = getFilesDir();
        File webuiDir = new File(files, "webui");
        File workspaceDir = new File(files, "workspace");
        File dataDir = new File(workspaceDir, "data");
        copyAssets("webui", webuiDir);
        workspaceDir.mkdirs();
        dataDir.mkdirs();

        // 后台线程起本地 server(localhost:8090),生命周期内常驻
        final String webuiPath = webuiDir.getAbsolutePath();
        final String wsPath = workspaceDir.getAbsolutePath();
        final String dataPath = dataDir.getAbsolutePath();
        new Thread(() -> startServer(webuiPath, wsPath, dataPath, 8090), "hermes-server").start();

        // WebView 加载本地 server 提供的 WebUI,像 Windows 的 WebView2 一样点图标即用
        webView = new WebView(this);
        WebSettings s = webView.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        s.setSupportZoom(false);          // 禁用缩放(需三行一起,防双指/系统按钮缩放)
        s.setBuiltInZoomControls(false);
        s.setDisplayZoomControls(false);
        webView.setWebViewClient(new WebViewClient());
        // 文件树长按 → "用其他应用打开"(FileBridge 转 Android Intent)
        webView.addJavascriptInterface(new FileBridge(this, workspaceDir), "AndroidBridge");
        setContentView(webView);

        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            webView.loadUrl("http://127.0.0.1:8090/");
        }, 1500);
    }

    @Override
    public void onBackPressed() {
        if (webView != null && webView.canGoBack()) { webView.goBack(); return; }
        super.onBackPressed();
    }

    // 递归复制 assets 子目录到私有目录
    private void copyAssets(String src, File dst) {
        try {
            String[] list = getAssets().list(src);
            if (list == null || list.length == 0) {
                dst.getParentFile().mkdirs();
                try (InputStream in = getAssets().open(src);
                     FileOutputStream out = new FileOutputStream(dst)) {
                    byte[] buf = new byte[8192];
                    int n;
                    while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
                }
            } else {
                dst.mkdirs();
                for (String child : list) copyAssets(src + "/" + child, new File(dst, child));
            }
        } catch (Exception e) {
            Log.e("Hermes", "copyAssets failed: " + e);
        }
    }
}
