// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertThrows;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.content.Context;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.webkit.MimeTypeMap;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;

/** The data folder in the Files app (DataProvider): document IDs and paths, and nothing outside the folder reached */
@RunWith(RobolectricTestRunner.class)
public class DataProviderTest {
    @Rule public TemporaryFolder outside = new TemporaryFolder();

    private Context context;
    private DataProvider provider;
    private File data;

    @Before
    public void setUp() throws IOException {
        context = RuntimeEnvironment.getApplication();
        // the manifest's declaration, which DocumentsProvider checks (exported, MANAGE_DOCUMENTS)
        provider = Robolectric.buildContentProvider(DataProvider.class).create().get();
        data = DataProvider.folder(context).getCanonicalFile();
        assertTrue(new File(data, "games/demo").mkdirs());
        write(new File(data, "games/demo/demo.nro"), "nro");
        assertTrue(new File(data, "state").mkdirs());
        write(new File(data, "state/scarletnx.log"), "a log");
    }

    private static void write(File file, String text) throws IOException {
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static String read(File file) throws IOException {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }

    private Cursor document(String id) throws FileNotFoundException {
        Cursor cursor = provider.queryDocument(id, null);
        assertTrue(id + " has a row", cursor.moveToFirst());
        assertEquals(1, cursor.getCount());
        return cursor;
    }

    private static String text(Cursor cursor, String column) {
        return cursor.getString(cursor.getColumnIndexOrThrow(column));
    }

    private static int flags(Cursor cursor) {
        return cursor.getInt(cursor.getColumnIndexOrThrow(Document.COLUMN_FLAGS));
    }

    @Test
    public void theManifestDeclaresTheAuthorityTheCodeUses() {
        ProviderInfo info = context.getPackageManager().resolveContentProvider(DataProvider.authority(context), 0);
        assertNotNull("org.scarletnx.data in the manifest", info);
        assertEquals(DataProvider.class.getName(), info.name);
        assertEquals("org.scarletnx.data", DataProvider.authority(context));
    }

    @Test
    public void theDataFolderIsScarletnxInTheAppsFiles() throws IOException {
        assertEquals(new File(context.getFilesDir(), ".scarletnx").getCanonicalPath(), data.getPath());
        assertTrue(data.isDirectory());
    }

    @Test
    public void oneRootTheFolderItself() {
        Cursor roots = provider.queryRoots(null);
        assertEquals(1, roots.getCount());
        assertTrue(roots.moveToFirst());
        assertEquals("data", text(roots, Root.COLUMN_ROOT_ID));
        assertEquals("data", text(roots, Root.COLUMN_DOCUMENT_ID));
        assertEquals("ScarletNX", text(roots, Root.COLUMN_TITLE));
        assertEquals(Root.FLAG_SUPPORTS_CREATE | Root.FLAG_SUPPORTS_IS_CHILD | Root.FLAG_LOCAL_ONLY,
                     roots.getInt(roots.getColumnIndexOrThrow(Root.COLUMN_FLAGS)));
        assertEquals("content://org.scarletnx.data/root/data", DataProvider.rootUri(context).toString());
    }

    @Test
    public void theRootDocumentIsTheFolderAndCannotGo() throws FileNotFoundException {
        Cursor root = document("data");
        assertEquals("data", text(root, Document.COLUMN_DOCUMENT_ID));
        assertEquals("ScarletNX", text(root, Document.COLUMN_DISPLAY_NAME));
        assertEquals(Document.MIME_TYPE_DIR, text(root, Document.COLUMN_MIME_TYPE));
        assertEquals(Document.FLAG_DIR_SUPPORTS_CREATE, flags(root));
    }

    @Test
    public void aDocumentsIdIsItsPathAfterData() throws FileNotFoundException {
        Cursor folder = document("data/games/demo");
        assertEquals("data/games/demo", text(folder, Document.COLUMN_DOCUMENT_ID));
        assertEquals("demo", text(folder, Document.COLUMN_DISPLAY_NAME));
        assertEquals(Document.MIME_TYPE_DIR, text(folder, Document.COLUMN_MIME_TYPE));
        assertEquals(Document.FLAG_DIR_SUPPORTS_CREATE | Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME,
                     flags(folder));
        assertTrue(folder.isNull(folder.getColumnIndexOrThrow(Document.COLUMN_SIZE)));

        Cursor file = document("data/games/demo/demo.nro");
        assertEquals("demo.nro", text(file, Document.COLUMN_DISPLAY_NAME));
        assertEquals(Document.FLAG_SUPPORTS_WRITE | Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME,
                     flags(file));
        assertEquals(3, file.getLong(file.getColumnIndexOrThrow(Document.COLUMN_SIZE)));
    }

    @Test
    public void aFilesTypeComesFromItsExtension() throws IOException {
        shadowOf(MimeTypeMap.getSingleton()).addExtensionMimeTypeMapping("txt", "text/plain");
        write(new File(data, "README.TXT"), "");
        write(new File(data, "no-extension"), "");
        write(new File(data, "unknown.zzz"), "");
        assertEquals("text/plain", text(document("data/README.TXT"), Document.COLUMN_MIME_TYPE));
        assertEquals("application/octet-stream", text(document("data/no-extension"), Document.COLUMN_MIME_TYPE));
        assertEquals("application/octet-stream", text(document("data/unknown.zzz"), Document.COLUMN_MIME_TYPE));
    }

    @Test
    public void aMissingDocumentIsNotFound() {
        assertThrows(FileNotFoundException.class, () -> provider.queryDocument("data/games/none", null));
    }

    @Test
    public void anIdOutsideTheFolderIsRefused() throws IOException {
        File secret = outside.newFile("secret");
        File link = new File(data, "link");
        Files.createSymbolicLink(link.toPath(), outside.getRoot().toPath());
        for (String id : new String[] {"", "dat", "datax", "data/../..", "data/..", "data/../files", "games",
                                       "/data", "data/link", "data/link/secret"}) {
            FileNotFoundException refused =
                assertThrows(id, FileNotFoundException.class, () -> provider.queryDocument(id, null));
            assertEquals(id + " is not in the data folder", refused.getMessage());
            assertThrows(id, FileNotFoundException.class, () -> provider.openDocument(id, "r", null));
            assertThrows(id, FileNotFoundException.class, () -> provider.queryChildDocuments(id, null, (String)null));
        }
        assertTrue(secret.exists());
    }

    @Test
    public void aLinkThatStaysInTheFolderIsFollowed() throws IOException {
        Files.createSymbolicLink(new File(data, "logs").toPath(), new File(data, "state").toPath());
        assertEquals("data/state/scarletnx.log",
                     text(document("data/logs/scarletnx.log"), Document.COLUMN_DOCUMENT_ID));
    }

    @Test
    public void aFoldersChildrenAreItsFiles() throws FileNotFoundException {
        Cursor children = provider.queryChildDocuments("data", null, (String)null);
        List<String> ids = new ArrayList<>();
        while (children.moveToNext()) ids.add(text(children, Document.COLUMN_DOCUMENT_ID));
        ids.sort(null);
        assertEquals(Arrays.asList("data/games", "data/state"), ids);
        assertEquals(0, provider.queryChildDocuments("data/games/demo/demo.nro", null, (String)null).getCount());
    }

    @Test
    public void openDocumentReadsAndWritesTheFile() throws IOException {
        try (ParcelFileDescriptor in = provider.openDocument("data/state/scarletnx.log", "r", null);
             FileInputStream stream = new FileInputStream(in.getFileDescriptor())) {
            byte[] bytes = new byte[16];
            assertEquals(5, stream.read(bytes));
            assertEquals("a log", new String(bytes, 0, 5, StandardCharsets.UTF_8));
        }
        try (ParcelFileDescriptor out = provider.openDocument("data/state/scarletnx.log", "wt", null);
             FileOutputStream stream = new FileOutputStream(out.getFileDescriptor())) {
            stream.write("new".getBytes(StandardCharsets.UTF_8));
        }
        assertEquals("new", read(new File(data, "state/scarletnx.log")));
    }

    @Test
    public void aChildIsUnderItsParentNotBesideIt() throws IOException {
        assertTrue(new File(data, "gam").mkdir());
        assertTrue(provider.isChildDocument("data", "data/games/demo/demo.nro"));
        assertTrue(provider.isChildDocument("data/games", "data/games/demo"));
        assertFalse(provider.isChildDocument("data/games", "data/games"));
        assertFalse(provider.isChildDocument("data/gam", "data/games"));
        assertFalse(provider.isChildDocument("data/games", "data/state"));
        assertFalse(provider.isChildDocument("data/games", "data/games/../../x"));
    }

    @Test
    public void createDocumentMakesANewNameAndSaysSo() throws FileNotFoundException {
        assertEquals("data/state/notes.txt", provider.createDocument("data/state", "text/plain", "notes.txt"));
        assertEquals("data/state/notes.txt (1)", provider.createDocument("data/state", "text/plain", "notes.txt"));
        assertEquals("data/state/notes.txt (2)", provider.createDocument("data/state", "text/plain", "notes.txt"));
        assertEquals("data/saves", provider.createDocument("data", Document.MIME_TYPE_DIR, "saves"));
        assertTrue(new File(data, "state/notes.txt (1)").isFile());
        assertTrue(new File(data, "saves").isDirectory());
        List<Uri> notified = notified();
        assertTrue(notified.toString(), notified.contains(children("data/state")));
        assertTrue(notified.toString(), notified.contains(children("data")));
    }

    @Test
    public void createDocumentInAFolderThatIsNotThereFails() {
        FileNotFoundException failed = assertThrows(FileNotFoundException.class,
                                                    () -> provider.createDocument("data/none", "text/plain", "a"));
        assertEquals("cannot make " + new File(data, "none/a").getPath(), failed.getMessage());
    }

    @Test
    public void deleteDocumentTakesAFolderWholeButNotWhereItsLinksPoint() throws IOException {
        File kept = outside.newFile("kept");
        Files.createSymbolicLink(new File(data, "games/demo/out").toPath(), outside.getRoot().toPath());
        provider.deleteDocument("data/games");
        assertFalse(new File(data, "games").exists());
        assertTrue(kept.exists());
        assertTrue(notified().contains(children("data")));
    }

    @Test
    public void theRootCannotBeDeletedNorRenamed() {
        FileNotFoundException delete = assertThrows(FileNotFoundException.class, () -> provider.deleteDocument("data"));
        assertEquals("cannot delete data", delete.getMessage());
        FileNotFoundException rename =
            assertThrows(FileNotFoundException.class, () -> provider.renameDocument("data", "other"));
        assertEquals("cannot rename data to other", rename.getMessage());
        assertTrue(data.isDirectory());
    }

    @Test
    public void renameDocumentKeepsTheFolderAndRefusesAnotherOrATakenName() throws IOException {
        assertEquals("data/state/old.log", provider.renameDocument("data/state/scarletnx.log", "old.log"));
        assertEquals("a log", read(new File(data, "state/old.log")));
        assertFalse(new File(data, "state/scarletnx.log").exists());
        assertTrue(notified().contains(children("data/state")));
        for (String name : new String[] {"../games2", "sub/games2", "state"}) {
            FileNotFoundException refused =
                assertThrows(name, FileNotFoundException.class, () -> provider.renameDocument("data/games", name));
            assertEquals("cannot rename data/games to " + name, refused.getMessage());
        }
        assertTrue(new File(data, "games/demo/demo.nro").isFile());
        assertFalse(new File(data.getParentFile(), "games2").exists());
    }

    @Test
    public void documentUriNamesAFileOfTheFolderAndNothingElse() throws IOException {
        assertEquals("content://org.scarletnx.data/document/data%2Fstate%2Fscarletnx.log",
                     DataProvider.documentUri(context, new File(data, "state/scarletnx.log")).toString());
        assertEquals(DocumentsContract.buildDocumentUri("org.scarletnx.data", "data/games"),
                     DataProvider.documentUri(context, new File(data, "state/../games")));
        // the folder itself is the root's document, which MainActivity.openFolder names apart
        assertNull(DataProvider.documentUri(context, data));
        assertNull(DataProvider.documentUri(context, new File(data.getPath() + "-other/x")));
        assertNull(DataProvider.documentUri(context, outside.newFile("x.log")));
    }

    private Uri children(String id) {
        return DocumentsContract.buildChildDocumentsUri("org.scarletnx.data", id);
    }

    private List<Uri> notified() {
        List<Uri> uris = new ArrayList<>();
        for (org.robolectric.shadows.ShadowContentResolver.NotifiedUri notified :
             shadowOf(context.getContentResolver()).getNotifiedUris())
            uris.add(notified.uri);
        return uris;
    }
}
