"""Host checks for the release save provider; Android Binder/ADB still need a device check.

Run: python tests/test_save_provider.py --jdk <JDK folder>
The Android API stubs supply paths and caller identity only. The actual provider's
validation, hash checks, locking, backups and atomic commit run unchanged.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


STUBS = {
    "android/content/Context.java": """
package android.content;
public class Context {
    public static java.io.File root;
    public java.io.File getFilesDir() { return new java.io.File(root, "files"); }
    public java.io.File getCacheDir() { return new java.io.File(root, "cache"); }
}
""",
    "android/content/ContentProvider.java": """
package android.content;
public abstract class ContentProvider {
    public Context getContext() { return new Context(); }
    public abstract boolean onCreate();
    public abstract android.os.ParcelFileDescriptor openFile(android.net.Uri u, String m) throws java.io.FileNotFoundException;
    public abstract android.os.Bundle call(String m, String a, android.os.Bundle e);
    public abstract String getType(android.net.Uri u);
    public abstract android.database.Cursor query(android.net.Uri u, String[] p, String s, String[] a, String o);
    public abstract android.net.Uri insert(android.net.Uri u, ContentValues v);
    public abstract int delete(android.net.Uri u, String s, String[] a);
    public abstract int update(android.net.Uri u, ContentValues v, String s, String[] a);
}
""",
    "android/content/ContentValues.java": "package android.content; public class ContentValues {}",
    "android/database/Cursor.java": "package android.database; public interface Cursor {}",
    "android/net/Uri.java": """
package android.net;
public class Uri {
    private final String path;
    public Uri(String p) { path = p; }
    public java.util.List<String> getPathSegments() { return java.util.Arrays.asList(path.split("/")); }
}
""",
    "android/os/Binder.java": "package android.os; public class Binder { public static int uid=2000; public static int getCallingUid(){return uid;} }",
    "android/os/Process.java": "package android.os; public class Process { public static int myUid(){return 10001;} }",
    "android/os/Bundle.java": """
package android.os;
public class Bundle {
    private final java.util.Map<String,String> values = new java.util.HashMap<>();
    public String getString(String k){return values.get(k);}
    public void putString(String k,String v){values.put(k,v);}
}
""",
    "android/os/ParcelFileDescriptor.java": """
package android.os;
public class ParcelFileDescriptor {
    public static final int MODE_READ_ONLY=1, MODE_WRITE_ONLY=2, MODE_CREATE=4, MODE_TRUNCATE=8;
    public static ParcelFileDescriptor open(java.io.File f,int m) throws java.io.FileNotFoundException {
        if (m==MODE_READ_ONLY && !f.isFile()) throw new java.io.FileNotFoundException();
        return new ParcelFileDescriptor();
    }
}
""",
}

HARNESS = r"""
import com.rrjb.vr.SaveProvider;
import android.os.Bundle;
import android.os.Binder;
import android.net.Uri;
import java.nio.file.*;
import java.io.*;
import java.security.MessageDigest;

public class ProviderCheck {
    static SaveProvider provider = new SaveProvider();
    static Path root, target;
    static int checks;
    static void check(boolean condition, String name) {
        if (!condition) throw new AssertionError(name);
        ++checks;
    }
    static String hash(byte[] data) throws Exception {
        return java.util.HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(data));
    }
    static byte[] card(int marker) {
        byte[] data = new byte[131072];
        data[0]='M'; data[1]='C'; data[128]=0x51;
        for (int f=0; f<16; ++f) {
            int xor=0;
            for (int i=0;i<127;++i) xor^=data[f*128+i]&255;
            data[f*128+127]=(byte)xor;
        }
        data[8192]=(byte)marker;
        return data;
    }
    static String digest() throws Exception { return hash(Files.readAllBytes(target)); }
    static String token(int i) { return String.format("%032x",i); }
    static Bundle commit(byte[] data, int id, String expected, String checksum) throws Exception {
        Path dir=root.resolve("cache/save-transfer");
        Files.createDirectories(dir);
        Files.write(dir.resolve(token(id)+".mcr"),data);
        Bundle extra=new Bundle();
        extra.putString("token",token(id)); extra.putString("sha256",checksum); extra.putString("expected",expected);
        return provider.call("commit","career",extra);
    }
    static void rejected(byte[] data, int id, String expected, String checksum, String name) throws Exception {
        String before=digest();
        Bundle result=commit(data,id,expected,checksum);
        check("error".equals(result.getString("status")) && before.equals(digest()),name);
    }
    public static void main(String[] args) throws Exception {
        root=Path.of(args[0]); android.content.Context.root=root.toFile();
        Files.createDirectories(root.resolve("files")); target=root.resolve("files/saves/rrjb_card.mcr");
        check("missing".equals(provider.call("stat","career",null).getString("sha256")),"missing card");
        byte[] first=card(1), second=card(2);
        check("ok".equals(commit(first,1,"missing",hash(first)).getString("status")),"first import");
        check(hash(first).equals(digest()),"first readback");
        check("ok".equals(commit(second,2,hash(first),hash(second)).getString("status")),"replacement");
        check(hash(first).equals(hash(Files.readAllBytes(target.resolveSibling("rrjb_card.before-transfer-"+token(2)+".mcr")))),"backup");
        check(!Files.exists(root.resolve("cache/save-transfer/"+token(2)+".mcr")),"staging consumed");
        rejected(first,3,hash(first),hash(first),"changed destination");
        rejected(first,4,hash(second),hash(second),"wrong upload hash");
        rejected(new byte[12],5,hash(second),hash(first),"short upload");
        byte[] damaged=first.clone(); damaged[128]^=1;
        rejected(damaged,6,hash(second),hash(damaged),"directory corruption");
        byte[] empty=new byte[131072]; empty[0]='M';empty[1]='C';empty[127]=(byte)('M'^'C');
        rejected(empty,7,hash(second),hash(empty),"empty card");
        byte[] header=first.clone(); header[0]='X';header[127]^=(byte)('X'^'M');
        rejected(header,8,hash(second),hash(header),"wrong header");
        check("error".equals(provider.call("stat","../disc",null).getString("status")),"invalid card path");
        try { provider.openFile(new Uri("stage/../../disc"),"w"); throw new AssertionError(); }
        catch (FileNotFoundException expected) { ++checks; }
        try { provider.openFile(new Uri("card/career"),"w"); throw new AssertionError(); }
        catch (FileNotFoundException expected) { ++checks; }
        try (RandomAccessFile guard=new RandomAccessFile(root.resolve("files/save-transfer.lock").toFile(),"rw");
             java.nio.channels.FileLock lock=guard.getChannel().lock()) {
            check("error".equals(provider.call("stat","career",null).getString("status")),"busy save lock");
        }
        Binder.uid=12345;
        try { provider.call("stat","career",null); throw new AssertionError(); }
        catch (SecurityException expected) { ++checks; }
        Binder.uid=2000;
        check(hash(second).equals(provider.call("stat","career",null).getString("sha256")),"final readback");
        System.out.println(checks+"/"+checks+" provider host checks passed");
    }
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jdk", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="rrjb-save-provider-") as temporary:
        root = Path(temporary)
        for name, text in STUBS.items():
            destination = root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(text, encoding="utf-8")
        (root / "ProviderCheck.java").write_text(HARNESS, encoding="utf-8")
        source = repo / "android/app/src/main/java/com/rrjb/vr/SaveProvider.java"
        subprocess.run([str(args.jdk / "bin/javac.exe"), "-d", str(root), str(source)] +
                       [str(path) for path in root.rglob("*.java")], check=True)
        subprocess.run([str(args.jdk / "bin/java.exe"), "-cp", str(root), "ProviderCheck",
                        str(root / "runtime")], check=True)


if __name__ == "__main__":
    main()
