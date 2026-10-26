# Coffeeclaw

Herramienta externa para ARK: Survival Ascended. Lee memoria y dibuja encima.
No inyecta nada.

## Estado

Un solo build y **una funcionalidad activa**: el wallhack.

| Funcion | Estado |
|---|---|
| Nombres de dinosaurios | funciona |
| Distancia en metros | funciona |
| Lineas al centro (opcional) | funciona |
| Rango maximo configurable | funciona |
| Cajas por criatura | comentado en `src\main.cpp` |
| Esqueleto | comentado en `src\main.cpp` |
| Noclip / freecam | no incluido |

## Estructura

```
Cheat\
  src\
    main.cpp      panel, overlay, hilo lector y dibujo
    config.h      contrato de offsets.ini
    ue.h          recorrido del grafo UE5 (world, actores, camara, mesh)
    proc.h        attach y lectura (NtReadVirtualMemory)
    overlay.h     ventana layered y primitivas GDI
    w2s.h         proyeccion mundo -> pantalla
    pattern.h     firmas de bytes
    common.h      tipos base
  build\
    coffeeclaw.exe
    find_roots.exe      (herramienta, no se distribuye)
    offsets.ini
  tools\
    find_roots.cpp          extractor de offsets
    build_find_roots.cmd    compila el extractor
    find_roots_admin.cmd    lo ejecuta elevado
  build.cmd
  offsets.ini
  README.md
```

## Compilar

```
build.cmd
```

Sale `build\coffeeclaw.exe` y copia el `offsets.ini` al lado.

## Uso

```
build\coffeeclaw.exe      (administrador)
```

| Tecla | Accion |
|---|---|
| F1 | enciende / apaga el wallhack |
| F2 | recarga `offsets.ini` |
| F3 | diagnostico |

Requiere ARK en **ventana completa (borderless)**.

## Raices por firma

`GWorld` y `GNames` se resuelven por patron de bytes, no por direccion fija:

```ini
GWorldPattern=48 8B 05 ?? ?? ?? ?? 48 85 C0 74 18 48 8B 88 A8 04 00 00
GNamesPattern=4C 8D 05 ?? ?? ?? ?? 4C 89 BC 24 C0 00 00 00
```

Unicas en el binario, resueltas por RIP. Extraidas con Ghidra 12.1.4 sobre
`ArkAscended.exe` build 94.34.0.

## Re-extraer offsets tras un parche de ARK

```
tools\build_find_roots.cmd
tools\find_roots_admin.cmd
```

Deja el reporte en `found_roots.txt`.

## Cajas y esqueleto

Estan **comentados** en `src\main.cpp`, marcados con `[DESACTIVADO]`:

- dibujo de cajas (`main.cpp`, sobre la linea 350)
- dibujo del esqueleto (sobre la 362)
- creacion de los dos checkboxes (sobre la 482)

Se reactivan quitando el comentario de esos bloques. El esqueleto ademas
necesita el array animado: `mesh+0x6D8`, paso `0x60`, cuaternion double en `+0`,
traslacion en `+32`, escala en `+64`.

## Notas tecnicas

- Las lecturas usan `NtReadVirtualMemory` con caida a `ReadProcessMemory`.
- El overlay es una ventana layered click-through que se pega al area de cliente
  del juego y se oculta cuando este pierde el foco.
- El proceso se abre solo lectura: ninguna escritura en el juego.

## Freecam de camara (pendiente, con avances)

Objetivo: mover solo el punto de vista, sin tocar al personaje. Eso si seria
invisible para el servidor. Estado actual:

- Manual mapping probado: `build\mapper.exe` + `payload\payload_hook.dll` carga
  codigo dentro de ARK sin `LoadLibrary` (bloqueado por el juego).
- Hook de la instruccion del POV instalado en `0x66AEF62` -> `ok=1`, y el codigo
  se ejecuta (contador subiendo).
- **Pero esa instruccion no es la del frame**: se ejecuta 2 veces en 45 s, no
  ~75 por segundo. Parchearla no mueve la camara.

Siguiente paso, con depurador (no a ciegas): breakpoint de hardware de escritura
sobre `CameraManager+0x540` y quedarse con la instruccion que mas se repite.

## Herramientas de desarrollo

```
tools\find_roots.cpp        extractor de offsets
tools\mapper.cpp            manual mapping (experimental)
payload\payload_hook.cpp    payload del hook de camara (experimental)
build\freecam_hook.cmd      instala el hook y mapea el payload
```
