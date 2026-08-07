package com.hermes.agent;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * C++ 出站 HTTP 的 Java 桥:用系统 HttpURLConnection(自带 TLS)发起请求。
 * C++ 端循环调 readChunk 逐块拉取流式响应,无反向回调。
 */
public class HermesHttp {

    public static class HttpStream {
        final HttpURLConnection conn;
        final int code;
        final InputStream input;

        HttpStream(HttpURLConnection c, int code, InputStream in) {
            conn = c;
            this.code = code;
            input = in;
        }
    }

    public static HttpStream open(String method, String url, String headers, byte[] body,
                                  int connectTimeoutMs, int readTimeoutMs) throws Exception {
        HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
        conn.setRequestMethod(method);
        conn.setConnectTimeout(connectTimeoutMs);
        conn.setReadTimeout(readTimeoutMs);
        conn.setInstanceFollowRedirects(true);
        if (headers != null && !headers.isEmpty()) {
            // "kvk2v2"
            String[] parts = headers.split("");
            for (String p : parts) {
                if (p.isEmpty()) continue;
                String[] kv = p.split("", 2);
                conn.setRequestProperty(kv[0], kv.length > 1 ? kv[1] : "");
            }
        }
        if (body != null && body.length > 0) {
            conn.setDoOutput(true);
            conn.getOutputStream().write(body);
        }
        int code = conn.getResponseCode();
        InputStream is = (code >= 400) ? conn.getErrorStream() : conn.getInputStream();
        return new HttpStream(conn, code, is);
    }

    public static int readChunk(HttpStream s, byte[] buf) throws Exception {
        return s.input.read(buf);
    }

    public static void close(HttpStream s) {
        try { if (s.input != null) s.input.close(); } catch (Exception ignored) {}
        try { s.conn.disconnect(); } catch (Exception ignored) {}
    }

    public static int status(HttpStream s) {
        return s.code;
    }

    public static String getHeader(HttpStream s, String name) {
        return s.conn.getHeaderField(name);
    }
}
