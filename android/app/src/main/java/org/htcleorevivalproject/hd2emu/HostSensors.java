package org.htcleorevivalproject.hd2emu;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;

import org.json.JSONObject;

/**
 * Sensors mapping (HLE): this phone's real accelerometer, magnetometer and
 * light sensor, fed into the emulated HD2's chips while it runs.
 *
 * The machine has three QOM properties (htcleo_i2cdev.c): accel "X,Y,Z" in
 * milli-g (the BMA150 behind the microP), mag "X,Y,Z" in microtesla (AKM8973)
 * and light LUX (the microP ADC).  Both guests read those chips through the
 * same I2C paths, so Windows Mobile and Android see the same live data.  The
 * axes already agree: Android's device frame (x right, y up, z out of the
 * screen, +1 g on z when flat) is the HD2 driver's too.
 *
 * Gyroscope and proximity have no chip in the HD2 (the HD2 has a proximity
 * bit in the microP but the driver reads none), so nothing to map there.
 */
public class HostSensors implements SensorEventListener {
    private static final long MIN_INTERVAL_MS = 50;   // 20 Hz is plenty for an I2C-polled chip

    private final SensorManager sm;
    private final Qmp qmp;
    private final long[] last = new long[3];
    private final float[] lastLux = {-1};

    public HostSensors(Context ctx, Qmp qmp) {
        this.sm = (SensorManager) ctx.getSystemService(Context.SENSOR_SERVICE);
        this.qmp = qmp;
    }

    public void start() {
        int[] types = {Sensor.TYPE_ACCELEROMETER, Sensor.TYPE_MAGNETIC_FIELD, Sensor.TYPE_LIGHT};
        for (int t : types) {
            Sensor s = sm.getDefaultSensor(t);
            if (s != null) sm.registerListener(this, s, SensorManager.SENSOR_DELAY_GAME);
        }
    }

    public void stop() {
        sm.unregisterListener(this);
    }

    private void set(String prop, Object value) {
        try {
            JSONObject a = new JSONObject();
            a.put("path", "/machine");
            a.put("property", prop);
            a.put("value", value);
            qmp.exec("qom-set", a, new Qmp.Reply());
        } catch (Exception ignored) {
        }
    }

    @Override
    public void onSensorChanged(SensorEvent e) {
        long now = System.currentTimeMillis();
        int slot;
        switch (e.sensor.getType()) {
            case Sensor.TYPE_ACCELEROMETER: slot = 0; break;
            case Sensor.TYPE_MAGNETIC_FIELD: slot = 1; break;
            case Sensor.TYPE_LIGHT: slot = 2; break;
            default: return;
        }
        if (now - last[slot] < MIN_INTERVAL_MS) return;
        last[slot] = now;
        float[] v = e.values;
        if (slot == 0) {
            float g = SensorManager.GRAVITY_EARTH / 1000f;
            set("accel", Math.round(v[0] / g) + "," + Math.round(v[1] / g) + "," + Math.round(v[2] / g));
        } else if (slot == 1) {
            set("mag", Math.round(v[0]) + "," + Math.round(v[1]) + "," + Math.round(v[2]));
        } else if (v[0] != lastLux[0]) {
            lastLux[0] = v[0];
            set("light", Math.round(v[0]));
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
    }
}
