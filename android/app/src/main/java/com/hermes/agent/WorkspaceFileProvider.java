package com.hermes.agent;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.List;

/**
 * 把工作区文件以 content:// 暴露给外部 App(供"用其他应用打开")。
 * 不引入 androidx 依赖的自实现 Provider:按路径段解析,严格限定在工作区内。
 */
public class WorkspaceFileProvider extends ContentProvider {
    public static final String AUTHORITY = "com.hermes.agent.files";

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public String getType(Uri uri) {
        String name = uri.getLastPathSegment();
        String ext = null;
        if (name != null) {
            int dot = name.lastIndexOf('.');
            if (dot >= 0) ext = name.substring(dot + 1).toLowerCase();
        }
        String mime = ext == null ? null
                : MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
        return mime != null ? mime : "application/octet-stream";
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        File base = new File(getContext().getFilesDir(), "workspace");
        StringBuilder rel = new StringBuilder();
        for (String seg : uri.getPathSegments()) {
            if (seg.equals("..") || seg.contains("/") || seg.contains("\\"))
                throw new FileNotFoundException("bad path segment");
            if (rel.length() > 0) rel.append(File.separatorChar);
            rel.append(seg);
        }
        File file = new File(base, rel.toString());
        String baseCanon;
        String fileCanon;
        try {
            baseCanon = base.getCanonicalPath();
            fileCanon = file.getCanonicalPath();
        } catch (Exception e) {
            throw new FileNotFoundException("resolve failed");
        }
        if (!file.isFile() || !fileCanon.startsWith(baseCanon + File.separator))
            throw new FileNotFoundException("outside workspace");
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) { return null; }

    @Override
    public int delete(Uri uri, String selection, String[] args) { return 0; }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] args, String sortOrder) { return null; }
}
