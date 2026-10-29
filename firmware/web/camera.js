let streamUrl = document.location.origin + ':81'

function readParam(el) {
	if (el.type === 'checkbox')
		return el.checked ? 1 : 0
	return el.value
}

function setParam(el, value) {
	value = ~~value
	if (el.type === 'checkbox')
		el.checked = !!value
	else
		el.value = value
}

function netError(err) {
	if (err == 0 || err == 404)
		toast({ txt: 'There is an error in the network, please check.', type: 'error' })
}

function syncExpUi() {
	let aec = document.getElementById('aec_sensor')
	let agc = document.getElementById('agc')
	let aecOn = aec && aec.checked
	let agcOn = agc && agc.checked
	document.querySelectorAll('.exp-manual').forEach(function (el) {
		el.classList.toggle('hidden', aecOn)
	})
	document.querySelectorAll('.exp-auto').forEach(function (el) {
		el.classList.toggle('hidden', !aecOn)
	})
	document.querySelectorAll('.agc-manual').forEach(function (el) {
		el.classList.toggle('hidden', agcOn)
	})
	document.querySelectorAll('.agc-auto').forEach(function (el) {
		el.classList.toggle('hidden', !agcOn)
	})
}

function updateConfig(el) {
	let data = {}
	data[el.id] = String(readParam(el))
	ajax({
		method: 'post',
		url: '/set_video_info',
		data: data,
		error: function (err) {
			netError(err)
			if (err != 0 && err != 404) {
				toast({ txt: 'The camera did not accept that setting', type: 'error' })
				getData()
			}
		},
		success: function () {
			if (el.id === 'dummy_lines' || el.id === 'resolution' || el.id === 'clkrc' ||
				el.id === 'aec_sensor')
				getData()
		}
	})
	if (el.id === 'aec_sensor' || el.id === 'agc')
		syncExpUi()
}

function getData() {
	ajax({
		url: '/get_video_info',
		success: function (res) {
			document.querySelectorAll('.params-val').forEach(function (el) {
				setParam(el, res[el.id])
			})
			syncExpUi()
			if (window.applyExposureCaps) applyExposureCaps(res)
		},
		error: netError
	})
}

function startStream() {
	document.querySelector('#stream').src = streamUrl + '/stream'
}

function saveVideo() {
	let ids = [
		'resolution', 'brightness', 'contrast', 'saturation', 'h_mirror', 'v_flip',
		'aec_sensor', 'exposure', 'aec_dsp', 'ae_level', 'agc', 'agc_gain',
		'gain_ceiling', 'clkrc', 'dummy_lines'
	]
	let params = {}
	ids.forEach(function (id) {
		let el = document.getElementById(id)
		if (!el)
			return
		params[id] = el.type === 'checkbox' ? (el.checked ? 1 : 0) : Number(el.value)
	})
	ajax({
		method: 'post',
		url: '/set_video_cfg',
		data: params,
		success: function (res) {
			if (res && res.status === 0) {
				toast({ txt: res.msg || 'Failed', type: 'error' })
				return
			}
			let msg = res && res.msg && res.msg !== 'ok' ? res.msg : 'Saved'
			toast({ txt: msg, type: 'success' })
		},
		error: function () {
			toast({ txt: 'Could not save video settings', type: 'error' })
		}
	})
}

document.querySelectorAll('.params-val').forEach(function (el) {
	el.onchange = function () { updateConfig(el) }
})

getData()
startStream()
syncExpUi()
