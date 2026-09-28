function drawMask() {
	let m = document.querySelector('#mask')
	m.style.left = (Number(num('#sky_x')) || 0) + '%'
	m.style.top = (Number(num('#sky_y')) || 0) + '%'
	m.style.width = (Number(num('#sky_w')) || 0) + '%'
	m.style.height = (Number(num('#sky_h')) || 0) + '%'
}

;['#sky_x', '#sky_y', '#sky_w', '#sky_h'].forEach(function (id) {
	document.querySelector(id).addEventListener('input', drawMask)
})

let maskDrag = null

function maskCursor(mode) {
	if (mode === 'e' || mode === 'w')
		return 'ew-resize'
	if (mode === 'n' || mode === 's')
		return 'ns-resize'
	if (mode === 'ne' || mode === 'sw')
		return 'nesw-resize'
	if (mode === 'nw' || mode === 'se')
		return 'nwse-resize'
	return 'move'
}

function maskEdge(ev, box) {
	let edge = 12
	if (box.width < edge * 3 || box.height < edge * 3)
		return 'move'
	let horiz = ''
	let vert = ''
	if (ev.clientX - box.left < edge)
		horiz = 'w'
	else if (box.right - ev.clientX < edge)
		horiz = 'e'
	if (ev.clientY - box.top < edge)
		vert = 'n'
	else if (box.bottom - ev.clientY < edge)
		vert = 's'
	return (vert + horiz) || 'move'
}

function maskHas(mode, ch) {
	return mode !== 'move' && mode.indexOf(ch) >= 0
}

function maskWrite(x, y, w, h) {
	document.querySelector('#sky_x').value = String(x)
	document.querySelector('#sky_y').value = String(y)
	document.querySelector('#sky_w').value = String(w)
	document.querySelector('#sky_h').value = String(h)
	drawMask()
}

function maskFromDrag(dx, dy) {
	let mode = maskDrag.mode
	let x = maskDrag.x
	let y = maskDrag.y
	let right = maskDrag.x + maskDrag.w
	let bottom = maskDrag.y + maskDrag.h
	if (mode === 'move') {
		x = Math.round(maskDrag.x + dx)
		y = Math.round(maskDrag.y + dy)
		if (x < 0)
			x = 0
		if (y < 0)
			y = 0
		if (x > 100 - maskDrag.w)
			x = 100 - maskDrag.w
		if (y > 100 - maskDrag.h)
			y = 100 - maskDrag.h
		return [x, y, maskDrag.w, maskDrag.h]
	}
	if (maskHas(mode, 'e'))
		right = Math.round(maskDrag.x + maskDrag.w + dx)
	if (maskHas(mode, 'w'))
		x = Math.round(maskDrag.x + dx)
	if (maskHas(mode, 's'))
		bottom = Math.round(maskDrag.y + maskDrag.h + dy)
	if (maskHas(mode, 'n'))
		y = Math.round(maskDrag.y + dy)
	if (x < 0)
		x = 0
	if (y < 0)
		y = 0
	if (right > 100)
		right = 100
	if (bottom > 100)
		bottom = 100
	if (right < x + 1) {
		if (maskHas(mode, 'w'))
			x = right - 1
		else
			right = x + 1
	}
	if (bottom < y + 1) {
		if (maskHas(mode, 'n'))
			y = bottom - 1
		else
			bottom = y + 1
	}
	return [x, y, right - x, bottom - y]
}

let maskEl = document.querySelector('#mask')
maskEl.addEventListener('pointerdown', function (ev) {
	if (ev.button !== 0)
		return
	let still = document.querySelector('.sky-still').getBoundingClientRect()
	if (still.width < 2 || still.height < 2)
		return
	let box = maskEl.getBoundingClientRect()
	maskDrag = {
		mode: maskEdge(ev, box),
		px: ev.clientX,
		py: ev.clientY,
		rw: still.width,
		rh: still.height,
		x: Number(num('#sky_x')) || 0,
		y: Number(num('#sky_y')) || 0,
		w: Number(num('#sky_w')) || 1,
		h: Number(num('#sky_h')) || 1
	}
	maskEl.style.cursor = maskCursor(maskDrag.mode)
	maskEl.setPointerCapture(ev.pointerId)
	ev.preventDefault()
})
maskEl.addEventListener('pointermove', function (ev) {
	if (!maskDrag) {
		maskEl.style.cursor = maskCursor(maskEdge(ev, maskEl.getBoundingClientRect()))
		return
	}
	let dx = (ev.clientX - maskDrag.px) / maskDrag.rw * 100
	let dy = (ev.clientY - maskDrag.py) / maskDrag.rh * 100
	let next = maskFromDrag(dx, dy)
	maskWrite(next[0], next[1], next[2], next[3])
})
function maskDragEnd() {
	maskDrag = null
}
maskEl.addEventListener('pointerup', maskDragEnd)
maskEl.addEventListener('pointercancel', maskDragEnd)
