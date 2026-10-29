// Exposure limits, labels and night / day switching for the Camera page.

let expCaps = null

function el(id) { return document.getElementById(id) }

function aecMaxFor(res) {
	let dummy = Number(el('dummy_lines').value) || 0
	let fs = String(el('resolution') ? el('resolution').value : '')
	let by = res && res.aec_base_lines
	if (by && by[fs] != null)
		return by[fs] + dummy
	return (res && res.aec_max) || 1200
}

function syncExposureLabels() {
	let max = expCaps ? aecMaxFor(expCaps) : 1200
	let slider = el('exposure')
	slider.max = String(max)
	if (Number(slider.value) > max)
		slider.value = String(max)
	el('exposure_max').textContent = String(max)
	el('exposure_val').textContent = slider.value
	el('agc_gain_val').textContent = el('agc_gain').value
	el('dummy_lines_val').textContent = el('dummy_lines').value
}

function applyExposureCaps(res) {
	expCaps = res
	let ov = res.supports_timing !== 0
	document.querySelectorAll('.timing-only').forEach(function (e) {
		e.classList.toggle('hidden', !ov)
	})
	let sel = el('gain_ceiling')
	if (sel && sel.options.length && res.gain_ceiling_default != null) {
		let names = ['2x', '4x', '8x', '16x', '32x', '64x', '128x']
		let d = res.gain_ceiling_default
		sel.options[0].textContent = d >= 0
			? 'Auto (firmware default: ' + names[d] + ')'
			: 'Auto (driver default)'
	}
	let lo = res.ae_level_min != null ? res.ae_level_min : -2
	let hi = res.ae_level_max != null ? res.ae_level_max : 2
	el('ae_level').min = String(lo)
	el('ae_level').max = String(hi)
	let est = res.est_frame_ms != null ? res.est_frame_ms : '—'
	let meas = res.meas_frame_ms
	el('expEstHint').textContent = 'Estimated frame time: ~' + est + ' ms' +
		(meas ? ' (measured ' + meas + ' ms)' : '')
	syncExposureLabels()
}

;['exposure', 'agc_gain', 'dummy_lines'].forEach(function (id) {
	el(id).addEventListener('input', syncExposureLabels)
})

function autoSceneText(scene) {
	if (!scene) return 'no sample yet'
	if (scene.status === 'ok' && scene.ev != null) return 'EV ' + Number(scene.ev).toFixed(1)
	if (scene.status === 'clipped_high') return 'clipped high'
	if (scene.status === 'clipped_low') return 'clipped low'
	return 'unavailable'
}

function autoSwitchText(sw) {
	if (!sw) return 'no switch yet'
	let text = sw.ok ? 'switched to ' + (sw.to || '?') : 'switch to ' + (sw.to || '?') + ' failed'
	text += ' (' + (sw.trigger || '?') + ')'
	if (sw.ev != null) text += ' at EV ' + Number(sw.ev).toFixed(1)
	if (sw.message) text += ': ' + sw.message
	if (sw.ageS != null) text += ' · ' + sw.ageS + 's ago'
	return text
}

function autoStatusText(res) {
	if (res.available === false) return 'Automatic switching needs an OV2640 sensor'
	let luma = res.lastLuma ? 'luma ' + res.lastLuma.avg : 'no luma'
	if (res.lastSampleAgeS != null) luma += ' · ' + res.lastSampleAgeS + 's ago'
	let n = res.samples != null ? res.samples : 0
	return 'Mode: ' + (res.mode || 'unknown') + ' · ' + autoSceneText(res.lastScene) +
		' · ' + luma + ' · low ' + (res.lowCount || 0) + '/' + n +
		' high ' + (res.highCount || 0) + '/' + n + ' · ' + autoSwitchText(res.lastSwitch) +
		' · ' + (res.hasNight ? 'night preset saved' : 'no night preset')
}

let autoFormDirty = false

function applyAutoForm(res) {
	let ids = ['autoEnabled', 'autoLow', 'autoHigh', 'autoInterval', 'autoSamples', 'autoDwell', 'saveAutoBtn']
	ids.forEach(function (id) { el(id).disabled = res.available === false })
	if (!autoFormDirty) {
		el('autoEnabled').checked = !!res.enabled
		if (res.lowEv != null) el('autoLow').value = res.lowEv
		if (res.highEv != null) el('autoHigh').value = res.highEv
		if (res.intervalMin != null) el('autoInterval').value = res.intervalMin
		if (res.samples != null) el('autoSamples').value = res.samples
		if (res.minDwellMin != null) el('autoDwell').value = res.minDwellMin
	}
	el('autoStatus').textContent = autoStatusText(res)
}

function loadAutoStatus() {
	ajax({
		url: '/api/exposure/auto',
		success: function (res) { if (res && res.ok) applyAutoForm(res) }
	})
}

function postAuto(body, reload) {
	ajax({
		method: 'post',
		url: '/api/exposure/auto',
		data: body,
		success: function (res) {
			toast({ txt: res.message || (res.ok ? 'OK' : 'Failed'), type: res.ok ? 'success' : 'error' })
			if (!res.ok) return
			if (body.action === 'settings') autoFormDirty = false
			applyAutoForm(res)
			if (reload) setTimeout(function () { getData(); loadAutoStatus() }, 6000)
		},
		error: function () { toast({ txt: 'Request failed', type: 'error' }) }
	})
}

;['autoEnabled', 'autoLow', 'autoHigh', 'autoInterval', 'autoSamples', 'autoDwell'].forEach(function (id) {
	el(id).addEventListener('input', function () { autoFormDirty = true })
	el(id).addEventListener('change', function () { autoFormDirty = true })
})

el('saveNightBtn').onclick = function () { postAuto({ action: 'saveNight' }, false) }
el('applyNightBtn').onclick = function () { postAuto({ action: 'applyNight' }, true) }
el('applyDayBtn').onclick = function () { postAuto({ action: 'applyDay' }, true) }
el('saveAutoBtn').onclick = function () {
	postAuto({
		action: 'settings',
		enabled: el('autoEnabled').checked,
		lowEv: parseFloat(el('autoLow').value),
		highEv: parseFloat(el('autoHigh').value),
		intervalMin: parseInt(el('autoInterval').value, 10),
		samples: parseInt(el('autoSamples').value, 10),
		minDwellMin: parseInt(el('autoDwell').value, 10)
	}, false)
}

loadAutoStatus()
setInterval(loadAutoStatus, 15000)
