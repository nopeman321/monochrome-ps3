/**
 * Monochrome Web Client — Lossless Music Player Application Logic
 * Supports dynamic Tidal/Monochrome API instance switching, failover, and local proxy.
 */

const API_INSTANCES = [
    { url: 'https://tidal.qqdl.site', label: 'tidal.qqdl.site' },
    { url: 'https://monochrome.tf', label: 'monochrome.tf' },
    { url: 'https://monochrome.samidy.com', label: 'monochrome.samidy.com' },
    { url: 'https://wolf.qqdl.site', label: 'wolf.qqdl.site' },
    { url: 'https://tracks.monochrome.st', label: 'tracks.monochrome.st' },
    { url: 'http://127.0.0.1:3000', label: 'Local Proxy (127.0.0.1:3000)' }
];
let currentInstanceIndex = 0;

function getApiBase() {
    return API_INSTANCES[currentInstanceIndex].url;
}

function setApiInstanceByUrl(url) {
    const idx = API_INSTANCES.findIndex(i => i.url === url);
    if (idx !== -1) {
        currentInstanceIndex = idx;
        updateInstanceUI();
    }
}

function updateInstanceUI() {
    const inst = API_INSTANCES[currentInstanceIndex];
    if (dom.instanceSelect) dom.instanceSelect.value = inst.url;
    if (dom.sidebarInstanceText) dom.sidebarInstanceText.textContent = inst.label;
}

/* ── Application State ───────────────────────────────────────────── */
const state = {
    tracks: [],
    currentIndex: -1,
    isPlaying: false,
    volume: 0.8,
    isMuted: false,
    currentQuery: 'trending'
};

/* ── DOM Element Cache ───────────────────────────────────────────── */
const dom = {
    audio: document.getElementById('audio-engine'),
    searchInput: document.getElementById('search-input'),
    searchClearBtn: document.getElementById('search-clear-btn'),
    cardsGrid: document.getElementById('cards-grid'),
    tracklistBody: document.getElementById('tracklist-body'),
    trackCountBadge: document.getElementById('track-count-badge'),
    sectionTitle: document.getElementById('section-title'),
    sectionSubtitle: document.getElementById('section-subtitle'),
    
    // Topbar & Instance Selector
    instanceSelect: document.getElementById('instance-select'),
    sidebarInstanceText: document.getElementById('sidebar-instance-text'),
    connectionIndicator: document.getElementById('connection-indicator'),
    liveDot: document.getElementById('live-dot'),
    liveText: document.getElementById('live-text'),
    
    // Visual Error / CORS Banner
    errorBanner: document.getElementById('error-banner'),
    errorText: document.getElementById('error-text'),
    btnErrorRetry: document.getElementById('btn-error-retry'),
    btnErrorDismiss: document.getElementById('btn-error-dismiss'),
    
    // Player controls
    btnPlayPause: document.getElementById('btn-play-pause'),
    btnPrev: document.getElementById('btn-prev'),
    btnNext: document.getElementById('btn-next'),
    btnShuffle: document.getElementById('btn-shuffle'),
    btnRepeat: document.getElementById('btn-repeat'),
    playIcon: document.getElementById('play-icon'),
    pauseIcon: document.getElementById('pause-icon'),
    
    // Now Playing bar
    npCoverImg: document.getElementById('np-cover-img'),
    npCoverPlaceholder: document.getElementById('np-cover-placeholder'),
    npTitle: document.getElementById('np-title'),
    npArtist: document.getElementById('np-artist'),
    audioSpecBadge: document.getElementById('audio-spec-badge'),
    
    // Timeline
    timeCurrent: document.getElementById('time-current'),
    timeTotal: document.getElementById('time-total'),
    progressTrack: document.getElementById('progress-track'),
    progressFill: document.getElementById('progress-fill'),
    progressThumb: document.getElementById('progress-thumb'),
    
    // Volume
    btnVolume: document.getElementById('btn-volume'),
    volHighIcon: document.getElementById('vol-high-icon'),
    volMuteIcon: document.getElementById('vol-mute-icon'),
    volumeTrack: document.getElementById('volume-track'),
    volumeFill: document.getElementById('volume-fill')
};

function showErrorBanner(message, showRetry = true) {
    if (!dom.errorBanner) return;
    dom.errorText.textContent = message;
    if (dom.btnErrorRetry) dom.btnErrorRetry.style.display = showRetry ? 'inline-block' : 'none';
    dom.errorBanner.style.display = 'flex';
    setConnectionStatus('CORS / OFFLINE', '#f59e0b');
}

function hideErrorBanner() {
    if (!dom.errorBanner) return;
    dom.errorBanner.style.display = 'none';
}

function setConnectionStatus(text, color) {
    if (dom.liveText) dom.liveText.textContent = text;
    if (dom.liveDot) dom.liveDot.style.backgroundColor = color;
    if (dom.connectionIndicator) {
        dom.connectionIndicator.style.borderColor = color;
        dom.connectionIndicator.style.color = color;
    }
}

/* ── Helper: Format Duration (seconds to mm:ss) ──────────────────── */
function formatTime(seconds) {
    if (isNaN(seconds) || seconds < 0) return '0:00';
    const m = Math.floor(seconds / 60);
    const s = Math.floor(seconds % 60);
    return `${m}:${s.toString().padStart(2, '0')}`;
}

/* ── Offline Fallback Catalogue ──────────────────────────────────── */
const FALLBACK_TRACKS = [
    {
        id: "155102881689636864",
        title: "Harder, Better, Faster, Stronger",
        artist: "Daft Punk",
        album: "Discovery",
        duration: 224,
        isHiRes: false,
        artwork: "https://tracks.monochrome.st/proxy/mi/155102851176075264-1A01.jpg"
    },
    {
        id: "155102871061270528",
        title: "One More Time",
        artist: "Daft Punk",
        album: "Discovery",
        duration: 320,
        isHiRes: false,
        artwork: "https://tracks.monochrome.st/proxy/mi/155102851176075264-1A01.jpg"
    },
    {
        id: "155142491052445696",
        title: "Instant Crush",
        artist: "Daft Punk ft. Julian Casablancas",
        album: "Random Access Memories",
        duration: 337,
        isHiRes: true,
        artwork: "https://tracks.monochrome.st/proxy/mi/155142475147644928-1A01.jpg"
    },
    {
        id: "155142502393843712",
        title: "Get Lucky",
        artist: "Daft Punk ft. Pharrell Williams",
        album: "Random Access Memories",
        duration: 248,
        isHiRes: true,
        artwork: "https://tracks.monochrome.st/proxy/mi/155142475147644928-1A01.jpg"
    }
];

/* ── Fetch Tracks & Releases from Upstream API ───────────────────── */
/* ── Fetch Tracks & Releases from Upstream API ───────────────────── */
async function searchTracks(query) {
    dom.trackCountBadge.textContent = 'Searching...';
    const cleanQ = query.trim() || 'trending';
    state.currentQuery = cleanQ;

    let successful = false;
    let lastError = null;

    // Try current instance, and failover across other instances if unreachable or CORS blocked
    for (let attempt = 0; attempt < API_INSTANCES.length; attempt++) {
        const instIndex = (currentInstanceIndex + attempt) % API_INSTANCES.length;
        const inst = API_INSTANCES[instIndex];
        const searchUrl = `${inst.url}/search?q=${encodeURIComponent(cleanQ)}`;

        try {
            const controller = new AbortController();
            const timeoutId = setTimeout(() => controller.abort(), 6000);

            const resp = await fetch(searchUrl, {
                signal: controller.signal,
                headers: { 'Accept': 'application/json' }
            });
            clearTimeout(timeoutId);

            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);

            const data = await resp.json();
            
            // Support tracks, data, items, or root array
            let rawTracks = data.tracks || data.data || data.items || [];
            if (rawTracks && typeof rawTracks === 'object' && !Array.isArray(rawTracks)) {
                rawTracks = rawTracks.items || rawTracks.tracks || [];
            }
            if (Array.isArray(data) && rawTracks.length === 0) {
                rawTracks = data;
            }

            if (Array.isArray(rawTracks) && rawTracks.length > 0) {
                state.tracks = rawTracks.map(t => {
                    let artistName = 'Unknown Artist';
                    if (Array.isArray(t.artistNames) && t.artistNames.length > 0) {
                        artistName = t.artistNames.join(', ');
                    } else if (Array.isArray(t.artists) && t.artists.length > 0) {
                        artistName = t.artists.map(a => typeof a === 'object' ? a.name : a).join(', ');
                    } else if (typeof t.artist === 'object' && t.artist) {
                        artistName = t.artist.name || 'Unknown Artist';
                    } else if (typeof t.artist === 'string') {
                        artistName = t.artist;
                    }

                    const dur = (t.duration > 1000) ? Math.round(t.duration / 1000) : (t.duration || 0);
                    const q = (t.audioQuality || t.quality || '');
                    const isHiRes = Boolean(q.includes('HI_RES') || q.includes('MASTER') || q.includes('96'));

                    let art = t.artwork || t.cover || t.picture || '';
                    if (!art && t.releaseId) {
                        art = `${inst.url}/proxy/mi/${t.releaseId}-1A01.jpg`;
                    }

                    let albumTitle = 'Single / Release';
                    if (typeof t.album === 'object' && t.album) albumTitle = t.album.title || t.album.name || albumTitle;
                    else if (typeof t.album === 'string') albumTitle = t.album;
                    else if (t.albumTitle) albumTitle = t.albumTitle;

                    return {
                        id: String(t.trackId || t.recordingId || t.id || Math.random()),
                        title: t.title || t.name || 'Untitled Track',
                        artist: artistName,
                        album: albumTitle,
                        duration: dur,
                        isHiRes: isHiRes,
                        artwork: art
                    };
                });

                currentInstanceIndex = instIndex;
                updateInstanceUI();
                setConnectionStatus('ONLINE', '#10b981');
                hideErrorBanner();
                successful = true;
                break;
            } else if (Array.isArray(rawTracks) && rawTracks.length === 0) {
                showErrorBanner(`No tracks found for "${cleanQ}". Showing trending tracks instead.`, false);
                state.tracks = FALLBACK_TRACKS;
                successful = true;
                break;
            }
        } catch (err) {
            console.warn(`[MONOCHROME] Search failed on ${inst.label}:`, err);
            lastError = err;
        }
    }

    if (!successful) {
        console.warn('[MONOCHROME] All live instances failed or CORS blocked. Engaging cached catalogue.');
        state.tracks = FALLBACK_TRACKS;
        showErrorBanner(`Instances unreachable or blocked by CORS (${lastError ? lastError.message : 'timeout'}). Using offline cached catalogue.`, true);
    }

    renderView();
}

/* ── Render Main Content View ────────────────────────────────────── */
function renderView() {
    dom.sectionTitle.textContent = state.currentQuery === 'trending' ? 'Trending & Featured' : `Results for "${state.currentQuery}"`;
    dom.sectionSubtitle.textContent = `${state.tracks.length} tracks available for lossless streaming`;
    dom.trackCountBadge.textContent = `${state.tracks.length} Tracks`;

    // 1. Render Featured Album Cards (First 4 unique releases)
    dom.cardsGrid.innerHTML = '';
    const featured = state.tracks.slice(0, 5);
    featured.forEach((track, idx) => {
        const card = document.createElement('div');
        card.className = 'card-item';
        card.innerHTML = `
            <div class="card-art-box">
                <img src="${track.artwork || ''}" alt="${track.title}" onerror="this.src='data:image/svg+xml;utf8,<svg xmlns=\\'http://www.w3.org/2000/svg\\' width=\\'100\\' height=\\'100\\'><rect width=\\'100\\' height=\\'100\\' fill=\\'%23161720\\'/><text x=\\'50%\\' y=\\'50%\\' font-size=\\'28\\' fill=\\'%23555\\' dominant-baseline=\\'middle\\' text-anchor=\\'middle\\'>♫</text></svg>'">
                <div class="card-play-overlay">
                    <svg viewBox="0 0 24 24" fill="currentColor"><polygon points="5 3 19 12 5 21 5 3"/></svg>
                </div>
            </div>
            <div class="card-title" title="${track.title}">${track.title}</div>
            <div class="card-artist" title="${track.artist}">${track.artist}</div>
        `;
        card.addEventListener('click', () => playTrack(idx));
        dom.cardsGrid.appendChild(card);
    });

    // 2. Render Tracklist Table Rows
    dom.tracklistBody.innerHTML = '';
    state.tracks.forEach((track, idx) => {
        const tr = document.createElement('tr');
        tr.className = `track-row ${idx === state.currentIndex ? 'active' : ''}`;
        tr.dataset.index = idx;

        const badgeHtml = track.isHiRes 
            ? `<span class="badge-hires">HI-RES 24-BIT</span>`
            : `<span class="badge-flac">FLAC 16-BIT</span>`;

        tr.innerHTML = `
            <td class="col-num">
                <span class="row-index">${idx + 1}</span>
                <svg class="row-play-btn" viewBox="0 0 24 24" fill="currentColor"><polygon points="5 3 19 12 5 21 5 3"/></svg>
            </td>
            <td class="col-cover">
                <img class="track-thumb" src="${track.artwork || ''}" alt="${track.title}" onerror="this.style.opacity='0.2'">
            </td>
            <td class="col-title">${escapeHtml(track.title)}</td>
            <td class="col-artist">${escapeHtml(track.artist)}</td>
            <td class="col-album">${escapeHtml(track.album)}</td>
            <td class="col-quality">${badgeHtml}</td>
            <td class="col-duration">${formatTime(track.duration)}</td>
        `;

        tr.addEventListener('click', () => playTrack(idx));
        dom.tracklistBody.appendChild(tr);
    });
}

function escapeHtml(str) {
    return String(str || '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

/* ── Audio Playback Pipeline ─────────────────────────────────────── */
function playTrack(index) {
    if (index < 0 || index >= state.tracks.length) return;
    state.currentIndex = index;
    const track = state.tracks[index];

    // Set Audio Source to active instance FLAC endpoint
    const streamUrl = `${getApiBase()}/track/${track.id}`;
    dom.audio.src = streamUrl;
    dom.audio.volume = state.isMuted ? 0 : state.volume;

    dom.audio.play().then(() => {
        state.isPlaying = true;
        hideErrorBanner();
        updatePlayerUI();
    }).catch(err => {
        console.warn('[PLAY ERROR]', err);
        showErrorBanner(`Audio playback failed on ${API_INSTANCES[currentInstanceIndex].label} (${err.name || 'CORS block'}). Switch instance or check proxy.`, true);
    });

    dom.audio.onerror = () => {
        showErrorBanner(`Audio stream failed to load from ${API_INSTANCES[currentInstanceIndex].label} (CORS / 404). Click 'Retry Fallback' to switch instance.`, true);
    };

    // Update Now-Playing Bar
    dom.npTitle.textContent = track.title;
    dom.npArtist.textContent = `${track.artist} — ${track.album}`;
    
    if (track.artwork) {
        dom.npCoverImg.src = track.artwork;
        dom.npCoverImg.style.display = 'block';
        dom.npCoverPlaceholder.style.display = 'none';
    } else {
        dom.npCoverImg.style.display = 'none';
        dom.npCoverPlaceholder.style.display = 'block';
    }

    dom.audioSpecBadge.textContent = track.isHiRes ? 'HI-RES 24/96 FLAC' : 'LOSSLESS FLAC';

    // Highlight row in table
    document.querySelectorAll('.track-row').forEach((r, i) => {
        r.classList.toggle('active', i === index);
    });
        dom.npCoverPlaceholder.style.display = 'block';
    }

    dom.audioSpecBadge.textContent = track.isHiRes ? 'HI-RES 24/96 FLAC' : 'LOSSLESS FLAC';

    // Highlight row in table
    document.querySelectorAll('.track-row').forEach((r, i) => {
        r.classList.toggle('active', i === index);
    });

    // Update Media Session (Windows Media Pop-up & Keyboard keys)
    if ('mediaSession' in navigator) {
        navigator.mediaSession.metadata = new MediaMetadata({
            title: track.title,
            artist: track.artist,
            album: track.album,
            artwork: track.artwork ? [{ src: track.artwork, sizes: '512x512', type: 'image/jpeg' }] : []
        });
    }
}

function togglePlayPause() {
    if (state.currentIndex === -1) {
        if (state.tracks.length > 0) playTrack(0);
        return;
    }

    if (dom.audio.paused) {
        dom.audio.play();
        state.isPlaying = true;
    } else {
        dom.audio.pause();
        state.isPlaying = false;
    }
    updatePlayerUI();
}

function playNext() {
    if (state.tracks.length === 0) return;
    const nextIdx = (state.currentIndex + 1) % state.tracks.length;
    playTrack(nextIdx);
}

function playPrev() {
    if (state.tracks.length === 0) return;
    if (dom.audio.currentTime > 3) {
        dom.audio.currentTime = 0;
        return;
    }
    const prevIdx = (state.currentIndex - 1 + state.tracks.length) % state.tracks.length;
    playTrack(prevIdx);
}

function updatePlayerUI() {
    if (state.isPlaying) {
        dom.playIcon.style.display = 'none';
        dom.pauseIcon.style.display = 'block';
    } else {
        dom.playIcon.style.display = 'block';
        dom.pauseIcon.style.display = 'none';
    }
}

/* ── Audio Engine Event Listeners ────────────────────────────────── */
dom.audio.addEventListener('timeupdate', () => {
    const cur = dom.audio.currentTime;
    const dur = dom.audio.duration;
    dom.timeCurrent.textContent = formatTime(cur);

    if (!isNaN(dur) && dur > 0) {
        dom.timeTotal.textContent = formatTime(dur);
        const pct = (cur / dur) * 100;
        dom.progressFill.style.width = `${pct}%`;
        dom.progressThumb.style.left = `${pct}%`;
    }
});

dom.audio.addEventListener('ended', () => {
    playNext();
});

dom.audio.addEventListener('play', () => {
    state.isPlaying = true;
    updatePlayerUI();
});

dom.audio.addEventListener('pause', () => {
    state.isPlaying = false;
    updatePlayerUI();
});

/* ── Scrubber & Volume Seek Controls ─────────────────────────────── */
dom.progressTrack.addEventListener('click', (e) => {
    if (isNaN(dom.audio.duration) || dom.audio.duration <= 0) return;
    const rect = dom.progressTrack.getBoundingClientRect();
    const ratio = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
    dom.audio.currentTime = ratio * dom.audio.duration;
});

dom.volumeTrack.addEventListener('click', (e) => {
    const rect = dom.volumeTrack.getBoundingClientRect();
    const vol = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
    state.volume = vol;
    state.isMuted = false;
    dom.audio.volume = vol;
    dom.volumeFill.style.width = `${vol * 100}%`;
    updateVolumeIcon();
});

dom.btnVolume.addEventListener('click', () => {
    state.isMuted = !state.isMuted;
    dom.audio.volume = state.isMuted ? 0 : state.volume;
    dom.volumeFill.style.width = `${state.isMuted ? 0 : state.volume * 100}%`;
    updateVolumeIcon();
});

function updateVolumeIcon() {
    if (state.isMuted || dom.audio.volume === 0) {
        dom.volHighIcon.style.display = 'none';
        dom.volMuteIcon.style.display = 'block';
    } else {
        dom.volHighIcon.style.display = 'block';
        dom.volMuteIcon.style.display = 'none';
    }
}

/* ── Search Input Handlers ───────────────────────────────────────── */
let searchDebounce = null;
dom.searchInput.addEventListener('input', (e) => {
    const val = e.target.value.trim();
    dom.searchClearBtn.style.display = val ? 'block' : 'none';

    clearTimeout(searchDebounce);
    searchDebounce = setTimeout(() => {
        searchTracks(val || 'trending');
    }, 350);
});

dom.searchInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
        clearTimeout(searchDebounce);
        searchTracks(dom.searchInput.value.trim() || 'trending');
    }
});

dom.searchClearBtn.addEventListener('click', () => {
    dom.searchInput.value = '';
    dom.searchClearBtn.style.display = 'none';
    searchTracks('trending');
});

/* ── Genre Pills & Navigation Handlers ───────────────────────────── */
document.querySelectorAll('.genre-pill').forEach(btn => {
    btn.addEventListener('click', () => {
        document.querySelectorAll('.genre-pill').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        const genre = btn.dataset.genre;
        dom.searchInput.value = '';
        dom.searchClearBtn.style.display = 'none';
        searchTracks(genre);
    });
});

document.querySelectorAll('.nav-item').forEach(btn => {
    btn.addEventListener('click', () => {
        document.querySelectorAll('.nav-item').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        
        if (btn.dataset.filter) {
            searchTracks(btn.dataset.filter);
        } else if (btn.dataset.nav === 'home') {
            searchTracks('trending');
        } else if (btn.dataset.nav === 'search') {
            dom.searchInput.focus();
        } else if (btn.dataset.nav === 'explore') {
            searchTracks('hi-res');
        }
    });
});

/* ── Media Controls ──────────────────────────────────────────────── */
dom.btnPlayPause.addEventListener('click', togglePlayPause);
dom.btnNext.addEventListener('click', playNext);
dom.btnPrev.addEventListener('click', playPrev);

/* ── Hardware Media Keys ─────────────────────────────────────────── */
if ('mediaSession' in navigator) {
    navigator.mediaSession.setActionHandler('play', togglePlayPause);
    navigator.mediaSession.setActionHandler('pause', togglePlayPause);
    navigator.mediaSession.setActionHandler('previoustrack', playPrev);
    navigator.mediaSession.setActionHandler('nexttrack', playNext);
}

/* ── Global Keyboard Shortcuts ───────────────────────────────────── */
window.addEventListener('keydown', (e) => {
    if (e.target.tagName === 'INPUT') return;
    
    if (e.code === 'Space') {
        e.preventDefault();
        togglePlayPause();
    } else if (e.code === 'ArrowRight') {
        dom.audio.currentTime = Math.min(dom.audio.duration, dom.audio.currentTime + 5);
    } else if (e.code === 'ArrowLeft') {
        dom.audio.currentTime = Math.max(0, dom.audio.currentTime - 5);
    } else if (e.key === '/' || e.key === 's') {
        e.preventDefault();
        dom.searchInput.focus();
        dom.searchInput.select();
    }
});

/* ── Instance Selector & Error Banner Handlers ───────────────────── */
if (dom.instanceSelect) {
    dom.instanceSelect.addEventListener('change', (e) => {
        setApiInstanceByUrl(e.target.value);
        hideErrorBanner();
        searchTracks(state.currentQuery || 'trending');
    });
}

if (dom.btnErrorRetry) {
    dom.btnErrorRetry.addEventListener('click', () => {
        hideErrorBanner();
        currentInstanceIndex = (currentInstanceIndex + 1) % API_INSTANCES.length;
        updateInstanceUI();
        searchTracks(state.currentQuery || 'trending');
    });
}

if (dom.btnErrorDismiss) {
    dom.btnErrorDismiss.addEventListener('click', () => {
        hideErrorBanner();
    });
}

/* ── Startup: Initialize Catalogue ───────────────────────────────── */
window.addEventListener('DOMContentLoaded', () => {
    updateInstanceUI();
    searchTracks('trending');
});
