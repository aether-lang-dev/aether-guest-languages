package ghl.rows;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Just enough JSON to read a host's answers: objects, arrays, strings, numbers, booleans, null. */
public final class Json {
    private final String s;
    private int i;

    private Json(String s) { this.s = s; }

    /** Parses {@code text}; throws IllegalArgumentException if it is not JSON. */
    public static Object parse(String text) {
        Json j = new Json(text);
        Object v = j.value();
        j.ws();
        if (j.i != j.s.length()) throw new IllegalArgumentException("trailing text");
        return v;
    }

    /** A JSON array of strings. */
    public static String array(String... items) {
        StringBuilder b = new StringBuilder("[");
        for (int k = 0; k < items.length; k++) {
            if (k > 0) b.append(',');
            b.append(string(items[k]));
        }
        return b.append(']').toString();
    }

    public static String string(String v) {
        StringBuilder b = new StringBuilder("\"");
        for (int k = 0; k < v.length(); k++) {
            char c = v.charAt(k);
            switch (c) {
                case '"': b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                default:
                    if (c < 0x20) b.append(String.format("\\u%04x", (int) c)); else b.append(c);
            }
        }
        return b.append('"').toString();
    }

    private void ws() { while (i < s.length() && Character.isWhitespace(s.charAt(i))) i++; }

    private IllegalArgumentException bad() { return new IllegalArgumentException("bad JSON at " + i); }

    private Object value() {
        ws();
        if (i >= s.length()) throw bad();
        char c = s.charAt(i);
        if (c == '"') return str();
        if (c == '{') {
            Map<String, Object> m = new LinkedHashMap<>();
            i++; ws();
            if (i < s.length() && s.charAt(i) == '}') { i++; return m; }
            while (true) {
                ws();
                String k = str();
                ws();
                if (i >= s.length() || s.charAt(i++) != ':') throw bad();
                m.put(k, value());
                ws();
                if (i >= s.length()) throw bad();
                char d = s.charAt(i++);
                if (d == '}') return m;
                if (d != ',') throw bad();
            }
        }
        if (c == '[') {
            List<Object> l = new ArrayList<>();
            i++; ws();
            if (i < s.length() && s.charAt(i) == ']') { i++; return l; }
            while (true) {
                l.add(value());
                ws();
                if (i >= s.length()) throw bad();
                char d = s.charAt(i++);
                if (d == ']') return l;
                if (d != ',') throw bad();
            }
        }
        if (s.startsWith("true", i)) { i += 4; return Boolean.TRUE; }
        if (s.startsWith("false", i)) { i += 5; return Boolean.FALSE; }
        if (s.startsWith("null", i)) { i += 4; return null; }
        int st = i;
        while (i < s.length() && "+-0123456789.eE".indexOf(s.charAt(i)) >= 0) i++;
        if (st == i) throw bad();
        return Double.valueOf(s.substring(st, i));
    }

    private String str() {
        if (i >= s.length() || s.charAt(i) != '"') throw bad();
        i++;
        StringBuilder b = new StringBuilder();
        while (i < s.length()) {
            char c = s.charAt(i++);
            if (c == '"') return b.toString();
            if (c != '\\') { b.append(c); continue; }
            if (i >= s.length()) throw bad();
            char e = s.charAt(i++);
            switch (e) {
                case 'n': b.append('\n'); break;
                case 'r': b.append('\r'); break;
                case 't': b.append('\t'); break;
                case 'b': b.append('\b'); break;
                case 'f': b.append('\f'); break;
                case 'u':
                    if (i + 4 > s.length()) throw bad();
                    b.append((char) Integer.parseInt(s.substring(i, i + 4), 16));
                    i += 4;
                    break;
                default: b.append(e);
            }
        }
        throw bad();
    }
}
