package org.htcleorevivalproject.hd2emu;

import android.app.Activity;
import android.os.Bundle;
import android.view.MenuItem;

/**
 * The screens behind the main one: the action bar's up caret goes back, and
 * the status bar is black, as on ICS.
 */
public abstract class HoloActivity extends Activity {
    protected Holo holo;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        holo = new Holo(this);
        Holo.blackBars(getWindow());
        if (getActionBar() != null) getActionBar().setDisplayHomeAsUpEnabled(true);
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        if (item.getItemId() == android.R.id.home) {
            finish();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }
}
