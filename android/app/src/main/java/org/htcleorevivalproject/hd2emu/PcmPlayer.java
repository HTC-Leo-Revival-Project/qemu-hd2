package org.htcleorevivalproject.hd2emu;

import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.util.Log;

import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;

/**
 * Plays the PCM that comes out of QEMU's VNC audio extension, the same stream
 * the browser gets in web/public/pcm-player.js, but through AudioTrack.
 *
 * One thread drains the queue into the track so a slow speaker never stalls
 * the picture: when the queue is full the oldest chunk goes instead.
 */
public class PcmPlayer {
    private static final String TAG = "HtcleoAudio";
    private static final int RATE = 48000;
    private static final int CHANNELS = 2;
    private static final int CHUNKS = 8;

    private final BlockingQueue<byte[]> queue = new ArrayBlockingQueue<byte[]>(CHUNKS);
    private volatile boolean running;
    private volatile float volume = 1f;
    private AudioTrack track;
    private Thread thread;

    public void start() {
        if (running) return;
        running = true;
        int min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT);
        if (min <= 0) min = 4096;
        int bytes = min * 4;                     // about 85 ms of stereo audio
        try {
            track = new AudioTrack(AudioManager.STREAM_MUSIC, RATE,
                    AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT,
                    bytes, AudioTrack.MODE_STREAM);
        } catch (IllegalArgumentException e) {
            Log.w(TAG, "no audio track: " + e);
            running = false;
            return;
        }
        track.setVolume(volume);
        track.play();
        thread = new Thread(new Runnable() {
            @Override
            public void run() {
                pump();
            }
        }, "audio");
        thread.setDaemon(true);
        thread.start();
    }

    public void stop() {
        running = false;
        if (thread != null) {
            try {
                thread.join(500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            thread = null;
        }
        if (track != null) {
            try {
                track.pause();
                track.flush();
                track.stop();
            } catch (IllegalStateException ignored) {
            }
            track.release();
            track = null;
        }
        queue.clear();
    }

    public void setVolume(float v) {
        this.volume = Math.max(0f, Math.min(1f, v));
        AudioTrack t = track;
        if (t != null) t.setVolume(this.volume);
    }

    /** Called from the VNC thread; never blocks for long. */
    public void write(byte[] pcm, int length) {
        if (!running) return;
        byte[] chunk = new byte[length];
        System.arraycopy(pcm, 0, chunk, 0, length);
        if (!queue.offer(chunk)) {
            queue.poll();                        // too far behind: drop the oldest
            queue.offer(chunk);
        }
    }

    private void pump() {
        while (running) {
            byte[] chunk;
            try {
                chunk = queue.poll(200, java.util.concurrent.TimeUnit.MILLISECONDS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
            if (chunk == null) continue;
            AudioTrack t = track;
            if (t == null) continue;
            int off = 0;
            while (off < chunk.length && running) {
                int n = t.write(chunk, off, chunk.length - off);
                if (n < 0) break;
                off += n;
            }
        }
    }
}
