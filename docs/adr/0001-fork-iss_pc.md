# ADR 0001 — Fork de `iss_pc` como base del port

- **Fecha:** 2026-10-19
- **Estado:** aceptado
- **Contexto:** Windows 11, i5-13420H, 16 GB RAM, RTX 3050 Laptop 4 GB.

## Contexto

Existen tres caminos para llevar inFAMOUS Second Son a PC:

1. **Port nativo desde cero**, usando `gow3_pc` como referencia.
2. **Fork de `HayatoG/iss_pc`**, que ya implementa el port nativo del mismo juego y la misma
   versión (CUSA00223 1.00 + 1.07).
3. **Seguir con shadPS4** (emulación) y optimizar el fork existente del usuario.

`iss_pc` usa la misma arquitectura que el port de God of War III: ejecución nativa x86-64,
runtime HLE específico del juego y núcleo Vulkan de shadPS4. Está en desarrollo activo,
es GPL-2.0 y ya es jugable hasta “Go to Longhouse”.

El usuario ya tiene un fork de shadPS4 (`Emulador-SecondSon`) con mejoras validadas de
host-cache, readbacks y tracking de páginas, pero la emulación completa sigue siendo más
costosa que un port nativo.

## Decisión

Adoptar la **Ruta A híbrida**:

- **Producción:** fork de `iss_pc`.
- **Laboratorio GPU/perf:** el fork de shadPS4 existente, para validar ideas y comparar.
- **Referencia de aprendizaje:** `gow3_pc`, solo durante 1–2 semanas.
- **Upstream-first:** los cambios que sean genéricos se proponen a `iss_pc`/shadPS4 en lugar
  de mantener un fork privado permanente.

## Consecuencias

### Positivas

- Se parte de un port que ya arranca, carga el juego y llega a gameplay.
- Se heredan los fixes de GNM/Vulkan, audio, input y saves.
- El esfuerzo se concentra en el problema real del usuario: **4 GB de VRAM** y rendimiento.
- La licencia GPL-2.0 permite fork y contribución.

### Negativas / riesgos

- El upstream se mueve rápido: hay que hacer rebase con frecuencia.
- El proyecto depende de un mantenedor principal y de varios ports compartiendo `ps4_core`.
- 4 GB está por debajo del mínimo recomendado; puede que no sea viable sin degradar calidad.
- Cualquier distribución exige publicar el código y mantener atribuciones.

## Gate G0

Antes de invertir en mejoras:

1. Compilar en MSYS2 CLANG64.
2. Ejecutar 15–30 min en la RTX 3050 4 GB.
3. Medir FPS, frame time, VRAM, RAM, crashes y calidad visual.
4. Si no arranca o es injugable, replantear: volver a shadPS4, reducir resolución/calidad o
   limitar el alcance a una contribución upstream de memoria.
