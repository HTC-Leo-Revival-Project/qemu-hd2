package com.hd2bench;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;

/** Benchmark results, by section, kept across runs and app restarts. */
final class Results {
    static final String CPU = "CPU";
    static final String GPU = "GPU (OpenGL ES 2.0)";
    static final String NAND = "Internal storage (/data)";
    static final String SD = "SD card";
    static final String INFO = "Device";

    static final class Item {
        final String section, name, unit, value;
        final boolean higherBetter;

        Item(String section, String name, String value, String unit,
             boolean higherBetter) {
            this.section = section;
            this.name = name;
            this.value = value;
            this.unit = unit;
            this.higherBetter = higherBetter;
        }
    }

    private static final LinkedHashMap<String, Item> items =
            new LinkedHashMap<String, Item>();

    private Results() {
    }

    static synchronized void put(String section, String name, double v,
                                 String unit, boolean higherBetter) {
        String s;

        if (v < 0 || Double.isNaN(v) || Double.isInfinite(v)) {
            s = "failed";
        } else if (v >= 100) {
            s = String.format(Locale.US, "%.0f", v);
        } else if (v >= 10) {
            s = String.format(Locale.US, "%.1f", v);
        } else {
            s = String.format(Locale.US, "%.2f", v);
        }
        putText(section, name, s, unit, higherBetter);
    }

    static synchronized void putText(String section, String name, String value,
                                     String unit, boolean higherBetter) {
        android.util.Log.i("hd2bench", section + ": " + name + " = " + value
                           + " " + unit);
        items.put(section + "\u0001" + name,
                  new Item(section, name, value, unit, higherBetter));
    }

    static synchronized void clear(String section) {
        List<String> drop = new ArrayList<String>();

        for (String k : items.keySet()) {
            if (k.startsWith(section + "\u0001")) {
                drop.add(k);
            }
        }
        for (String k : drop) {
            items.remove(k);
        }
    }

    static synchronized List<Item> section(String section) {
        List<Item> out = new ArrayList<Item>();

        for (Item i : items.values()) {
            if (i.section.equals(section)) {
                out.add(i);
            }
        }
        return out;
    }

    /** One line per result, for pasting into a report. */
    static synchronized String asText() {
        StringBuilder b = new StringBuilder("HD2 Bench 1.0\n");
        String last = null;

        for (Item i : items.values()) {
            if (!i.section.equals(last)) {
                b.append("\n[").append(i.section).append("]\n");
                last = i.section;
            }
            b.append(i.name).append(": ").append(i.value);
            if (i.unit.length() > 0) {
                b.append(' ').append(i.unit);
            }
            b.append('\n');
        }
        return b.toString();
    }

    static synchronized void save(Context c) {
        SharedPreferences.Editor e =
                c.getSharedPreferences("results", 0).edit().clear();
        int n = 0;

        for (Item i : items.values()) {
            e.putString("r" + n++, i.section + "\u0001" + i.name + "\u0001"
                        + i.value + "\u0001" + i.unit + "\u0001"
                        + (i.higherBetter ? "1" : "0"));
        }
        e.putInt("n", n).commit();
    }

    static synchronized void load(Context c) {
        SharedPreferences p = c.getSharedPreferences("results", 0);
        int n = p.getInt("n", 0);

        items.clear();
        for (int k = 0; k < n; k++) {
            String[] f = p.getString("r" + k, "").split("\u0001", -1);

            if (f.length == 5) {
                putText(f[0], f[1], f[2], f[3], f[4].equals("1"));
            }
        }
    }
}
