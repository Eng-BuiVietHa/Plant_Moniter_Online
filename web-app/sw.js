const CACHE_NAME = 'nong-trai-pwa-v4';

// Danh sách các file tĩnh cần pre-cache
const ASSETS_TO_CACHE = [
  './',
  './index.html',
  './style.css',
  './script.js',
  './firebase-config.js',
  './manifest.json',
  './icon-192.png',
  './icon-512.png'
];

// 1. Sự kiện install: Tải và lưu toàn bộ file tĩnh vào Cache Storage
self.addEventListener('install', (event) =>{
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => {
      console.log('[SW] Đã lưu đệm thành công bộ khung tĩnh');
      return cache.addAll(ASSETS_TO_CACHE);
    })
  );
  self.skipWaiting();
});

// 2. Sự kiện activate: Xóa sạch bộ nhớ đệm (Cache) phiên bản cũ khi có cập nhật
self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((keys) => {
      return Promise.all(
        keys.map((key) => {
          if (key !== CACHE_NAME) {
            console.log('[SW] Xóa bộ nhớ đệm cũ:', key);
            return caches.delete(key);
          }
        })
      );
    })
  );
  self.clients.claim();
});

// 3. Sự kiện fetch: Phục vụ file từ Cache khi mất mạng
self.addEventListener('fetch', (event) => {
  const url = new URL(event.request.url);

  // Bỏ qua Firebase API/auth và mọi phương thức ghi
  if (url.hostname.endsWith('.firebaseio.com') || url.hostname.endsWith('.firebasedatabase.app') || url.hostname.includes('googleapis.com') || event.request.method !== 'GET') {
    return;
  }

  event.respondWith(
    caches.match(event.request).then((cachedResponse) => {
      if (cachedResponse) {
        // Tải cập nhật ngầm nếu có mạng
        fetch(event.request).then((networkResponse) => {
          if (networkResponse && networkResponse.status === 200) {
            caches.open(CACHE_NAME).then((cache)=> {
              cache.put(event.request, networkResponse);
            });
          }
        }).catch(() => {/* Ngoại lệ khi offline */});

        return cachedResponse;
      }

      // Tải từ mạng nếu chưa lưu đệm
      return fetch(event.request).catch(() => {
        if (event.request.mode === 'navigate') {
          return caches.match('./index.html');
        }
      });
    })
  );
});
