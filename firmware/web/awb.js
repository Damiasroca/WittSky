let liveAwb = null
let awbLoaded = false
let awbBusy = false
let awbSig = ''
let awbCalPrev = ''
let AWB_PRESET_N = 4

function paintLive(gains) {
	let cells = document.querySelectorAll('#awb_live [data-ch]')
	if (!gains || gains.length < 3) {
		liveAwb = null
		cells.forEach(function (c) { c.textContent = '—' })
		return
	}
	liveAwb = gains
	cells.forEach(function (c) { c.textContent = gains[Number(c.dataset.ch)] })
}

function fillAwbMode(res) {
	let sel = document.querySelector('#sky_awb')
	let modes = [
		['0', 'Auto'],
		['1', 'Sunny'],
		['2', 'Cloudy'],
		['3', 'Office'],
		['4', 'Home']
	]
	;(res.presets || []).forEach(function (p, i) {
		if (p.used)
			modes.push([String(5 + i), p.name])
	})
	sel.innerHTML = ''
	modes.forEach(function (m) {
		let opt = document.createElement('option')
		opt.value = m[0]
		opt.textContent = m[1]
		sel.appendChild(opt)
	})
	let want = String(res.sky_awb)
	if ([...sel.options].some(function (o) { return o.value === want }))
		sel.value = want
}

function presetTile(p, i) {
	let tile = document.createElement('div')
	tile.className = 'tile'
	let label = document.createElement('div')
	label.className = 'tile-label'
	label.textContent = p.name
	let value = document.createElement('div')
	value.className = 'tile-value awb-gains'
	;[['ch-r', 'R', p.r], ['ch-g', 'G', p.g], ['ch-b', 'B', p.b]].forEach(function (part) {
		let s = document.createElement('span')
		s.className = part[0]
		s.textContent = part[1] + ' ' + part[2]
		value.appendChild(s)
	})
	let btn = document.createElement('button')
	btn.type = 'button'
	btn.className = 'common-btn preset-del'
	btn.textContent = 'Delete'
	btn.onclick = function () { deletePreset(i) }
	tile.appendChild(label)
	tile.appendChild(value)
	tile.appendChild(btn)
	return tile
}

function renderPresets(res) {
	let presets = res.presets || []
	let box = document.querySelector('#awb_saved')
	box.innerHTML = ''
	let saved = 0
	presets.forEach(function (p, i) {
		if (!p.used)
			return
		saved++
		box.appendChild(presetTile(p, i))
	})
	let full = saved >= AWB_PRESET_N
	document.querySelector('#awb_none').classList.toggle('hidden', saved > 0)
	document.querySelector('#awb_full').classList.toggle('hidden', !full)
	document.querySelector('#awb_save').classList.toggle('hidden', full)
	document.querySelector('#awb_cal_btn').classList.toggle('hidden', full)
}

function paintCal(res) {
	let run = !!(res && res.cal_state === 'run')
	let msg = document.querySelector('#awb_cal_msg')
	let text = res && res.cal_msg ? res.cal_msg : ''
	msg.textContent = text
	msg.classList.toggle('hidden', !text)
	let presets = res && res.presets ? res.presets : []
	let full = presets.filter(function (p) { return p.used }).length >= AWB_PRESET_N
	document.querySelector('#awb_save').classList.toggle('hidden', full || run)
	document.querySelector('#awb_cal_btn').classList.toggle('hidden', full || run)
	document.querySelectorAll('#awb_saved button').forEach(function (btn) {
		btn.disabled = run
	})
	document.querySelector('#sky_awb').disabled = run
	if (awbCalPrev === 'run' && res && res.cal_state === 'ok') {
		document.querySelector('#awb_name').value = ''
		toast({ txt: res.cal_msg || 'Preset saved', type: 'success' })
	}
	if (awbCalPrev === 'run' && res && res.cal_state === 'fail')
		toast({ txt: res.cal_msg || 'Calibration failed', type: 'error' })
	awbCalPrev = res && res.cal_state ? res.cal_state : ''
}

function paintAwb(res) {
	paintLive(res && res.awb_gains)
	if (!res || res.sky_awb === undefined)
		return
	let sig = JSON.stringify(res.presets || [])
	if (!awbLoaded || sig !== awbSig) {
		awbSig = sig
		fillAwbMode(res)
		renderPresets(res)
	} else if (document.querySelector('#sky_awb').value !== String(res.sky_awb)) {
		let sel = document.querySelector('#sky_awb')
		if ([...sel.options].some(function (o) { return o.value === String(res.sky_awb) }))
			sel.value = String(res.sky_awb)
	}
	paintCal(res)
	if (awbLoaded)
		return
	awbLoaded = true
	document.querySelector('#sky_awb_r').value = res.sky_awb_r
	document.querySelector('#sky_awb_g').value = res.sky_awb_g
	document.querySelector('#sky_awb_b').value = res.sky_awb_b
}

function pollAwb() {
	if (awbBusy)
		return
	awbBusy = true
	ajax({
		url: '/get_awb',
		success: function (res) {
			awbBusy = false
			paintAwb(res)
		},
		error: function () {
			awbBusy = false
		}
	})
}

function awbNumbers() {
	return {
		r: Number(document.querySelector('#sky_awb_r').value),
		g: Number(document.querySelector('#sky_awb_g').value),
		b: Number(document.querySelector('#sky_awb_b').value)
	}
}

function awbPost(url, data, okTxt, failTxt, done) {
	ajax({
		method: 'post',
		url: url,
		data: data,
		success: function (res) {
			if (res && res.status === 0) {
				toast({ txt: res.msg || 'Failed', type: 'error' })
				return
			}
			toast({ txt: okTxt(res), type: 'success' })
			if (done)
				done()
			awbSig = ''
			pollAwb()
		},
		error: function () {
			toast({ txt: failTxt, type: 'error' })
		}
	})
}

function presetName() {
	let name = document.querySelector('#awb_name').value.trim()
	if (!name)
		toast({ txt: 'Name the preset', type: 'error' })
	return name
}

function saveAwb() {
	let nums = awbNumbers()
	awbPost('/set_awb', {
		sky_awb: Number(document.querySelector('#sky_awb').value),
		sky_awb_r: nums.r,
		sky_awb_g: nums.g,
		sky_awb_b: nums.b
	}, function () { return 'White balance applied' }, 'Could not apply white balance')
}

function savePreset() {
	let name = presetName()
	if (!name)
		return
	let nums = awbNumbers()
	awbPost('/save_awb_preset', { name: name, r: nums.r, g: nums.g, b: nums.b },
		function () { return 'Preset saved' }, 'Could not save the preset',
		function () { document.querySelector('#awb_name').value = '' })
}

function calibrateAwb() {
	let name = presetName()
	if (!name)
		return
	awbPost('/calibrate_awb', { name: name },
		function (res) { return (res && res.msg) || 'Calibrating' }, 'Could not start calibration')
}

function deletePreset(slot) {
	awbPost('/delete_awb_preset', { slot: slot },
		function () { return 'Preset deleted' }, 'Could not delete the preset')
}

function copyLiveAwb() {
	if (!liveAwb) {
		toast({ txt: 'Auto does not report its gains', type: 'error' })
		return
	}
	document.querySelector('#sky_awb_r').value = liveAwb[0]
	document.querySelector('#sky_awb_g').value = liveAwb[1]
	document.querySelector('#sky_awb_b').value = liveAwb[2]
	toast({ txt: 'Copied. Name the preset and save it.', type: 'success' })
}

document.querySelector('#sky_awb').addEventListener('change', saveAwb)
pollAwb()
setInterval(pollAwb, 1000)
