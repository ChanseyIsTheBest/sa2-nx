// nfsmw - PNG captures of the game image (see nfsmw_nativo_captura.h).
//
// The PNG is written uncompressed (deflate "stored" blocks): the SDK does not
// include stb_image_write and this is enough for testing. A 1280x720 capture
// takes about 2.8 MB.

#include "nfsmw_nativo_captura.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

REXCVAR_DEFINE_INT32(nfsmw_captura_cada_s, 0, "NFSMW",
                     "Guardar una captura PNG de la imagen del juego cada N segundos en capturas/ "
                     "junto al ejecutable (0 = nunca; solo para pruebas)")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(nfsmw_captura_max, 20, "NFSMW", "Maximo de capturas por ejecucion")
    .range(1, 1000);
// banjo-nx: a short video: from banjo_video_s seconds on, banjo_video_frames consecutive presented frames are kept in
// memory and then written to capturas/video_<s>s.rgba (raw R8G8B8X8, frames back to back) with a .txt beside it
// (width, height, measured fps). tools/make_video.sh turns it into an MP4 with ffmpeg.
REXCVAR_DEFINE_INT32(banjo_video_s, 0, "NFSMW", "Seconds after start to record a short video (0 = off)").range(0, 3600);
REXCVAR_DEFINE_INT32(banjo_video_frames, 90, "NFSMW", "Frames in the video (banjo_video_s)").range(1, 600);
REXCVAR_DEFINE_BOOL(banjo_video_half, true, "NFSMW", "Record the video at half resolution");
// banjo-nx: a long recording streamed to ffmpeg (PC only): from banjo_record_s seconds on, banjo_record_seconds of
// the presented image at banjo_record_fps on a fixed timeline (a late capture repeats the last frame, so the video
// keeps real time), encoded to capturas/<banjo_record_name>.mp4 with NVENC. For comparisons against reference footage.
REXCVAR_DEFINE_INT32(banjo_record_s, 0, "NFSMW", "Seconds after start to begin a streamed recording (0 = off)")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(banjo_record_seconds, 150, "NFSMW", "Length of the streamed recording in seconds").range(1, 3600);
REXCVAR_DEFINE_INT32(banjo_record_fps, 60, "NFSMW", "Frame rate of the streamed recording").range(1, 120);
REXCVAR_DEFINE_STRING(banjo_record_name, "record", "NFSMW", "File name (without .mp4) of the streamed recording");
REXCVAR_DEFINE_STRING(banjo_record_ffmpeg, "C:/ffmpeg/ffmpeg.exe", "NFSMW", "ffmpeg used by the streamed recording");
// banjo-nx: a burst of captures per tick, to see flicker and broken vertices that one frame can hide.
REXCVAR_DEFINE_INT32(banjo_captura_rafaga, 1, "NFSMW", "Capturas seguidas en cada tick (rafaga)").range(1, 30);
REXCVAR_DEFINE_INT32(banjo_captura_rafaga_ms, 40, "NFSMW", "Milisegundos entre las capturas de una rafaga")
    .range(1, 1000);

extern std::atomic<uint64_t> g_banjo_game_frames;  // banjo_me_hooks.cpp (D3DDevice_Swap)

namespace nfsmw::captura {
namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_parar = false;
std::thread g_hilo;

uint32_t Crc32(const uint8_t* datos, size_t n, uint32_t crc) {
  static const std::array<uint32_t, 256> tabla = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();
  for (size_t i = 0; i < n; ++i) {
    crc = tabla[(crc ^ datos[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

void PonerBe32(std::vector<uint8_t>& salida, uint32_t valor) {
  salida.push_back(uint8_t(valor >> 24));
  salida.push_back(uint8_t(valor >> 16));
  salida.push_back(uint8_t(valor >> 8));
  salida.push_back(uint8_t(valor));
}

void Chunk(std::vector<uint8_t>& salida, const char tipo[4], const std::vector<uint8_t>& datos) {
  PonerBe32(salida, uint32_t(datos.size()));
  const size_t inicio_tipo = salida.size();
  salida.insert(salida.end(), tipo, tipo + 4);
  salida.insert(salida.end(), datos.begin(), datos.end());
  const uint32_t crc =
      Crc32(salida.data() + inicio_tipo, salida.size() - inicio_tipo, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
  PonerBe32(salida, crc);
}

// RGBX (R8 G8 B8 X8, like rex::ui::RawImage) to 8-bit RGB PNG.
std::vector<uint8_t> CodificarPng(const rex::ui::RawImage& imagen) {
  const uint32_t ancho = imagen.width;
  const uint32_t alto = imagen.height;
  std::vector<uint8_t> crudo;
  crudo.reserve(size_t(alto) * (size_t(ancho) * 3 + 1));
  for (uint32_t y = 0; y < alto; ++y) {
    crudo.push_back(0);  // sin filtro
    const uint8_t* fila = imagen.data.data() + size_t(y) * imagen.stride;
    for (uint32_t x = 0; x < ancho; ++x) {
      crudo.push_back(fila[x * 4]);
      crudo.push_back(fila[x * 4 + 1]);
      crudo.push_back(fila[x * 4 + 2]);
    }
  }

  // zlib with "stored" blocks and Adler-32 at the end.
  std::vector<uint8_t> zlib = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t trozo = std::min<size_t>(65535, crudo.size() - pos);
    const bool ultimo = pos + trozo == crudo.size();
    zlib.push_back(ultimo ? 1 : 0);
    zlib.push_back(uint8_t(trozo));
    zlib.push_back(uint8_t(trozo >> 8));
    zlib.push_back(uint8_t(~trozo));
    zlib.push_back(uint8_t(~trozo >> 8));
    zlib.insert(zlib.end(), crudo.begin() + pos, crudo.begin() + pos + trozo);
    pos += trozo;
  } while (pos < crudo.size());
  uint32_t a = 1, b = 0;
  for (uint8_t byte : crudo) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  PonerBe32(zlib, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PonerBe32(ihdr, ancho);
  PonerBe32(ihdr, alto);
  ihdr.push_back(8);  // bits per channel
  ihdr.push_back(2);  // RGB
  ihdr.push_back(0);  // deflate
  ihdr.push_back(0);  // filtro adaptativo
  ihdr.push_back(0);  // sin entrelazado
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", zlib);
  Chunk(png, "IEND", {});
  return png;
}

void Bucle(Capturador capturar, int cada_s, int maximo) {
  const std::filesystem::path carpeta = rex::filesystem::GetExecutableFolder() / "capturas";
  const auto inicio = std::chrono::steady_clock::now();
  for (int n = 1; n <= maximo; ++n) {
    {
      std::unique_lock<std::mutex> cerrojo(g_mutex);
      if (g_cv.wait_for(cerrojo, std::chrono::seconds(cada_s), [] { return g_parar; })) {
        return;
      }
    }
    const int rafaga = REXCVAR_GET(banjo_captura_rafaga);
    for (int k = 0; k < rafaga; ++k) {
    if (k) std::this_thread::sleep_for(std::chrono::milliseconds(REXCVAR_GET(banjo_captura_rafaga_ms)));
    rex::ui::RawImage imagen;
    if (!capturar(imagen) || !imagen.width || !imagen.height) {
      REXLOG_WARN("[captura] {}: no hay imagen del juego que capturar", n);
      continue;
    }
    std::error_code ec;
    std::filesystem::create_directories(carpeta, ec);
    const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - inicio)
                              .count();
    char nombre[64];
    if (rafaga > 1)
      std::snprintf(nombre, sizeof(nombre), "captura_%03d_%04llds_%02d.png", n, static_cast<long long>(segundos), k);
    else
      std::snprintf(nombre, sizeof(nombre), "captura_%03d_%04llds.png", n, static_cast<long long>(segundos));
    const std::filesystem::path ruta = carpeta / nombre;
    const std::vector<uint8_t> png = CodificarPng(imagen);
    std::ofstream fichero(ruta, std::ios::binary);
    fichero.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    if (!fichero) {
      REXLOG_WARN("[captura] No se pudo escribir {}", ruta.string());
      continue;
    }
    REXLOG_INFO("[captura] {} ({}x{})", ruta.string(), imagen.width, imagen.height);
    }
  }
}

}  // namespace

std::thread g_hilo_video;

void Video(Capturador capturar, int desde_s, int fotogramas) {
  {
    std::unique_lock<std::mutex> cerrojo(g_mutex);
    if (g_cv.wait_for(cerrojo, std::chrono::seconds(desde_s), [] { return g_parar; })) return;
  }
  std::vector<std::vector<uint8_t>> cuadros;
  uint32_t ancho = 0, alto = 0;
  const auto inicio = std::chrono::steady_clock::now();
  uint64_t ultimo = g_banjo_game_frames.load(std::memory_order_relaxed);
  while (int(cuadros.size()) < fotogramas) {
    {
      std::lock_guard<std::mutex> cerrojo(g_mutex);
      if (g_parar) break;
    }
    // One capture per game frame (D3DDevice_Swap), a few ms after it so the presenter has the new image.
    const uint64_t ahora = g_banjo_game_frames.load(std::memory_order_relaxed);
    if (ahora == ultimo) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    ultimo = ahora;
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    rex::ui::RawImage imagen;
    if (!capturar(imagen) || !imagen.width || !imagen.height ||
        (ancho && (imagen.width != ancho || imagen.height != alto))) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    // Half resolution (every other pixel of every other row): 300 frames of 1280x720 fit in ~280 MB.
    const uint32_t paso = REXCVAR_GET(banjo_video_half) ? 2 : 1;
    ancho = imagen.width;
    alto = imagen.height;
    const uint32_t w = ancho / paso, h = alto / paso;
    std::vector<uint8_t> cuadro(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* fila = imagen.data.data() + size_t(y * paso) * imagen.stride;
      uint8_t* destino = cuadro.data() + size_t(y) * w * 4;
      if (paso == 1) {
        std::memcpy(destino, fila, size_t(w) * 4);
      } else {
        for (uint32_t x = 0; x < w; ++x) std::memcpy(destino + size_t(x) * 4, fila + size_t(x) * paso * 4, 4);
      }
    }
    cuadros.push_back(std::move(cuadro));
  }
  if (cuadros.empty()) return;
  const double segundos = std::chrono::duration<double>(std::chrono::steady_clock::now() - inicio).count();
  const double fps = segundos > 0 ? double(cuadros.size()) / segundos : 30.0;
  const std::filesystem::path carpeta = rex::filesystem::GetExecutableFolder() / "capturas";
  std::error_code ec;
  std::filesystem::create_directories(carpeta, ec);
  const std::string nombre = "video_" + std::to_string(desde_s) + "s";
  std::ofstream crudo(carpeta / (nombre + ".rgba"), std::ios::binary);
  for (const auto& cuadro : cuadros) crudo.write(reinterpret_cast<const char*>(cuadro.data()), std::streamsize(cuadro.size()));
  std::ofstream info(carpeta / (nombre + ".txt"));
  const uint32_t paso_final = REXCVAR_GET(banjo_video_half) ? 2 : 1;
  info << ancho / paso_final << " " << alto / paso_final << " " << fps << " " << cuadros.size() << '\n';
  REXLOG_INFO("[captura] video {}: {} frames {}x{} in {:.2f} s ({:.1f} fps)", nombre, cuadros.size(), ancho, alto,
              segundos, fps);
}

std::thread g_hilo_grabacion;

void Grabacion(Capturador capturar) {
#if defined(_WIN32)
  const int desde_s = REXCVAR_GET(banjo_record_s);
  {
    std::unique_lock<std::mutex> cerrojo(g_mutex);
    if (g_cv.wait_for(cerrojo, std::chrono::seconds(desde_s), [] { return g_parar; })) return;
  }
  const int fps = REXCVAR_GET(banjo_record_fps);
  const int64_t total = int64_t(REXCVAR_GET(banjo_record_seconds)) * fps;
  const auto periodo = std::chrono::nanoseconds(1000000000LL / fps);
  const std::filesystem::path carpeta = rex::filesystem::GetExecutableFolder() / "capturas";
  std::error_code ec;
  std::filesystem::create_directories(carpeta, ec);
  const std::string salida = (carpeta / (REXCVAR_GET(banjo_record_name) + ".mp4")).string();
  std::FILE* tubo = nullptr;
  uint32_t ancho = 0, alto = 0;
  std::vector<uint8_t> cuadro;
  int64_t escritos = 0, repetidos = 0;
  const auto inicio = std::chrono::steady_clock::now();
  for (int64_t k = 0; k < total; ++k) {
    {
      std::lock_guard<std::mutex> cerrojo(g_mutex);
      if (g_parar) break;
    }
    const auto objetivo = inicio + periodo * k;
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora < objetivo) std::this_thread::sleep_until(objetivo);
    bool nuevo = false;
    if (ahora < objetivo + periodo || cuadro.empty()) {  // behind schedule: repeat the last frame instead
      rex::ui::RawImage imagen;
      if (capturar(imagen) && imagen.width && imagen.height &&
          (!ancho || (imagen.width == ancho && imagen.height == alto))) {
        if (!tubo) {
          ancho = imagen.width;
          alto = imagen.height;
          // cmd.exe strips one outer pair of quotes from the whole line.
          const std::string orden = "\"\"" + REXCVAR_GET(banjo_record_ffmpeg) +
              "\" -loglevel error -y -f rawvideo -pix_fmt rgb0 -s " + std::to_string(ancho) + "x" +
              std::to_string(alto) + " -r " + std::to_string(fps) +
              " -i - -c:v h264_nvenc -preset p5 -rc vbr -cq 19 -b:v 0 -pix_fmt yuv420p \"" + salida + "\"\"";
          tubo = _popen(orden.c_str(), "wb");
          if (!tubo) {
            REXLOG_WARN("[captura] could not start ffmpeg: {}", orden);
            return;
          }
          cuadro.resize(size_t(ancho) * alto * 4);
          REXLOG_INFO("[captura] recording {}x{} at {} fps for {} s to {}", ancho, alto, fps, total / fps, salida);
        }
        for (uint32_t y = 0; y < alto; ++y)
          std::memcpy(cuadro.data() + size_t(y) * ancho * 4, imagen.data.data() + size_t(y) * imagen.stride,
                      size_t(ancho) * 4);
        nuevo = true;
      }
    }
    if (!tubo) continue;  // no image yet: the video starts with the first one
    if (!nuevo) ++repetidos;
    if (std::fwrite(cuadro.data(), 1, cuadro.size(), tubo) != cuadro.size()) break;
    ++escritos;
  }
  if (tubo) {
    _pclose(tubo);
    REXLOG_INFO("[captura] recording done: {} frames ({} repeated) -> {}", escritos, repetidos, salida);
  }
#else
  (void)capturar;
#endif
}

void Arrancar(std::function<rex::ui::Presenter*()> obtener_presentador) {
  Arrancar(Capturador([obtener_presentador](rex::ui::RawImage& imagen) {
    rex::ui::Presenter* presentador = obtener_presentador ? obtener_presentador() : nullptr;
    return presentador && presentador->CaptureGuestOutput(imagen);
  }));
}

void Arrancar(Capturador capturar) {
  if (REXCVAR_GET(banjo_record_s) > 0 && !g_hilo_grabacion.joinable()) {
    {
      std::lock_guard<std::mutex> cerrojo(g_mutex);
      g_parar = false;
    }
    g_hilo_grabacion = std::thread(Grabacion, capturar);
  }
  if (REXCVAR_GET(banjo_video_s) > 0 && !g_hilo_video.joinable()) {
    {
      std::lock_guard<std::mutex> cerrojo(g_mutex);
      g_parar = false;
    }
    g_hilo_video = std::thread(Video, capturar, int(REXCVAR_GET(banjo_video_s)),
                               int(REXCVAR_GET(banjo_video_frames)));
  }
  const int cada_s = REXCVAR_GET(nfsmw_captura_cada_s);
  if (cada_s <= 0 || g_hilo.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> cerrojo(g_mutex);
    g_parar = false;
  }
  g_hilo = std::thread(Bucle, std::move(capturar), cada_s,
                       int(REXCVAR_GET(nfsmw_captura_max)));
  REXLOG_INFO("[captura] Una captura cada {} s (maximo {})", cada_s,
              int(REXCVAR_GET(nfsmw_captura_max)));
}

void Parar() {
  {
    std::lock_guard<std::mutex> cerrojo(g_mutex);
    g_parar = true;
  }
  g_cv.notify_all();
  if (g_hilo.joinable()) {
    g_hilo.join();
  }
  if (g_hilo_video.joinable()) {
    g_hilo_video.join();
  }
  if (g_hilo_grabacion.joinable()) {
    g_hilo_grabacion.join();
  }
}

}  // namespace nfsmw::captura
