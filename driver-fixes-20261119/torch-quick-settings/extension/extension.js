import Atk from 'gi://Atk';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {QuickSettingsItem} from 'resource:///org/gnome/shell/ui/quickSettings.js';

const LED = Gio.File.new_for_path('/sys/class/leds/white:flash-1/brightness');
const HELPER = '/usr/local/libexec/opd2513-torch';

export default class TorchExtension extends Extension {
    enable() {
        const settings = Main.panel.statusArea.quickSettings;
        if (this._attach(settings))
            return;

        // GNOME 50 builds the indicators after loading its network modules.
        this._indicators = settings._indicators;
        this._addedId = this._indicators.connect('child-added', () => {
            if (this._attach(settings))
                this._disconnectAdded();
        });
    }

    _attach(settings) {
        const row = settings._system?.quickSettingsItems[0]?.get_child();
        const screenshot = row?.get_children().find(
            child => child.get_icon_name?.() === 'screenshooter-symbolic');
        if (!screenshot)
            return false;

        const icon = Gio.FileIcon.new(this.dir.get_child('torch-symbolic.svg'));
        this._button = new QuickSettingsItem({
            style_class: 'icon-button opd2513-torch',
            accessible_role: Atk.Role.TOGGLE_BUTTON,
            can_focus: true,
            child: new St.Icon({gicon: icon, style_class: 'system-status-icon'}),
        });
        row.insert_child_below(this._button, screenshot);
        this._button.connect('clicked', () => this._toggle());
        this._menu = settings.menu;
        this._openId = this._menu.connect('open-state-changed', () => this._watch());
        this._watch();
        return true;
    }

    _disconnectAdded() {
        if (this._addedId)
            this._indicators.disconnect(this._addedId);
        this._addedId = 0;
        this._indicators = null;
    }

    _watch() {
        if (this._timer) {
            GLib.source_remove(this._timer);
            this._timer = 0;
        }
        this._sync();
        if (this._menu.isOpen) {
            this._timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 500, () => {
                this._sync();
                return GLib.SOURCE_CONTINUE;
            });
        }
    }

    _sync() {
        let value = null;
        try {
            const [, bytes] = LED.load_contents(null);
            const text = new TextDecoder().decode(bytes).trim();
            if (/^\d+$/.test(text))
                value = Number(text);
        } catch {
            // The stock boot or a missing LED driver leaves the control hidden.
        }
        this._button.visible = value !== null;
        this._button.reactive = value !== null && !this._process;
        this._button.checked = value > 0;
        this._button.accessible_name = value > 0 ? '手电筒：已开启' : '手电筒：已关闭';
    }

    _toggle() {
        if (this._process)
            return;
        this._sync();
        if (!this._button.visible)
            return;
        const desiredOn = !this._button.checked;
        try {
            const process = Gio.Subprocess.new([HELPER, desiredOn ? 'on' : 'off'],
                Gio.SubprocessFlags.NONE);
            const button = this._button;
            this._process = process;
            this._sync();
            process.wait_check_async(null, (source, result) => {
                if (this._process === source)
                    this._process = null;
                if (this._button !== button)
                    return;
                try {
                    source.wait_check_finish(result);
                } catch (error) {
                    Main.notifyError('手电筒', error.message);
                }
                this._sync();
            });
        } catch (error) {
            Main.notifyError('手电筒', error.message);
        }
    }

    disable() {
        this._disconnectAdded();
        if (this._timer)
            GLib.source_remove(this._timer);
        if (this._openId)
            this._menu.disconnect(this._openId);
        this._timer = this._openId = 0;
        this._button?.destroy();
        this._button = this._menu = null;
    }
}
