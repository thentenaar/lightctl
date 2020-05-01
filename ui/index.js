
import ons from 'onsenui/esm';
import 'onsenui/esm/elements/ons-page';
import 'onsenui/esm/elements/ons-toolbar';
import 'onsenui/esm/elements/ons-switch';
import 'onsenui/esm/elements/ons-list';
import 'onsenui/esm/elements/ons-list-item';
import 'onsenui/esm/elements/ons-list-header';
import 'onsenui/esm/elements/ons-alert-dialog';
import 'onsenui/esm/elements/ons-toast';
import 'onsenui/css/onsenui-core.min.css';
import 'onsenui/css/onsen-css-components.min.css';

function utc_time(val) {
	let s = '';
	let d = new Date();
	let v = val.split(':');
	d.setHours(v[0], v[1]);

	if (d.getUTCHours() < 10) s += '0';
	s += d.getUTCHours() + ':';

	if (d.getUTCMinutes() < 10) s += '0';
	s += d.getUTCMinutes();
	return s;
}

function local_time(val) {
	let s = '';
	let d = new Date();
	let v = val.split(':');

	if (v[0] == v[1])
		return s;

	d.setUTCHours(v[0], v[1]);
	if (d.getHours() < 10) s += '0';
	s += d.getHours() + ':';

	if (d.getMinutes() < 10) s += '0';
	s += d.getMinutes();
	return s;
}

function update_state() {
	fetch('/status', { method: 'HEAD' }).then(function(r) {
		if (r.status != 299)
			throw Error('Unexpected status code');

		let sw       = document.querySelector('[component=light_switch]');
		let ssw      = document.querySelector('[component=schedule_switch]');
		let override = document.querySelector('[component=manual_override]');
		let on       = document.querySelector('input[name=time_on]');
		let off      = document.querySelector('input[name=time_off]');
		let state    = r.statusText.split('/');

		sw.checked   = state[1] == 'on';
		ssw.checked  = state[2] == 'on';
		on.value     = local_time(state[3]);
		off.value    = local_time(state[4]);
		on.disabled  = ssw.checked;
		off.disabled = ssw.checked;
		override.textContent = state[0];
	}).catch(function(e) {
		ons.notification.toast(
			'Failed to fetch the current state',
			{ timeout: 1500 }
		);
	});
}

ons.ready(function() {
	let sw  = document.querySelector('[component=light_switch]');
	let ssw = document.querySelector('[component=schedule_switch]');

	sw.addEventListener('change', function() {
		fetch(
			this.checked ? '/on' : '/off',
			{ method: 'HEAD' }
		).catch(function(e) {
			ons.notification.toast(
				'Failed to turn the lights ' +
				(sw.checked ? 'on' : 'off'),
				{ timeout: 1000 }
			);

			sw.checked = !sw.checked;
		});
	});

	ssw.addEventListener('change', function() {
		let uri = '/schedule';
		let on  = document.querySelector('input[name=time_on]');
		let off = document.querySelector('input[name=time_off]');

		if (this.checked && on.value != off.value) {
			uri += '/on?on=' + utc_time(on.value);
			uri += '&off=' + utc_time(off.value);
		} else {
			uri += '/off';
			this.checked = false;
			on.disabled  = this.checked;
			off.disabled = this.checked;
		}

		fetch(uri, { method: 'HEAD' }).catch(function(e) {
			ons.notification.toast(
				'Failed to ' + (ssw.checked ? 'en' : 'dis') +
				'able the schedule',
				{ timeout: 1000 }
			);

			ssw.checked = !ssw.checked;
			on.disabled  = ssw.checked;
			off.disabled = ssw.checked;
		});
	});

	update_state();
});

