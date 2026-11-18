# Fase 3: las 11 funciones del kernel en la Vita (propuesta de diseño, sin implementar)

Fecha: 2026-11-18. Estado: **propuesta para revisar con Diego**.

## Qué son y por qué no se traducen

goal2c traduce a C 9637 de las 9648 funciones de Jak 1. Las 11 que faltan manipulan directamente
la pila y los registros del procesador, así que no tienen equivalente en C portable:

| # | Función | Qué hace |
|---|---|---|
| 1 | `thread-suspend` (método 10 de cpu-thread) | Suspende el proceso: guarda su pila y sus registros y vuelve al kernel. |
| 2 | `thread-resume` (método 11 de cpu-thread) | Reanuda un proceso: devuelve su pila y sus registros y continúa donde se suspendió. |
| 3 | `return-from-thread` | El código de un hilo termina: vuelve al kernel. |
| 4 | `return-from-thread-dead` | Igual, pero el proceso muere: vuelve al kernel y lo desactiva. |
| 5 | `reset-and-call` | Ejecuta una función en un hilo con la pila vacía (hilos trans/post, listener). |
| 6 | `set-to-run-bootstrap` | Primer código de un hilo recién preparado con `set-to-run`: toma los argumentos y llama a la función. |
| 7 | `new` de catch-frame (método 0) | `catch`: guarda el contexto y llama a una función. Si alguien hace `throw`, se vuelve aquí. |
| 8 | `throw-dispatch` | `throw`: salto no local al catch-frame. |
| 9 | `enter-state` (`.add`/`.jr`) | Cambio de estado (`go`) en el hilo principal: vacía la pila del proceso y salta al código del nuevo estado. |
| 10 | `deactivate` de process (`.push`/`.ret`) | Si el proceso se mata a sí mismo, salta a `return-from-thread`. |
| 11 | `compute-alignment!` de align-control (`.add`) | Lleva un `go` en línea (mismo salto que `enter-state`). |

En el PC (modo mixto) siguen en x86. 1 y 2 ya tienen versión nativa: el suspend con copia de pila,
validado con el recorrido oficial. En la Vita no hay x86: las 11 necesitan implementación nativa.

## Restricción clave: la pila de C debe estar dentro de la memoria GOAL

El código GOAL crea variables en la pila (`new 'stack`): en C, arrays locales (`g2c_stk`). Su
dirección GOAL es `dirección real - g2c_mem` y tiene que caber en 32 bits. Por tanto, **todo el
código GOAL (en C) debe ejecutarse sobre pilas que estén dentro del bloque de memoria GOAL**, igual
que en el PC:

- **Procesos:** sobre `*dram-stack*` (32 KB, en el heap global), como el juego original.
- **Kernel:** sobre la pila del kernel de GOAL (`GOAL_STACK_TOP`, en la cima de la memoria EE).

El hilo del EE de la Vita (pila de VitaSDK) solo ejecuta el runtime en C/C++ y entra en GOAL
cambiando de pila, igual que hace hoy `call_goal_on_stack` en el PC. Esto ya funciona así en el PC
con todo en C: los marcos de C viven en `*dram-stack*` (máximo medido: ~3 KB al suspender).

## Propuesta: tres primitivas nativas y las 11 funciones en C encima

### Primitivas (ARM, unas 120 líneas de ensamblador en total)

1. **`g2c_ctx_save(ctx)` / `g2c_ctx_jump(ctx, valor)`**: un setjmp/longjmp mínimo. Guarda r4-r11,
   sp y lr y los VFP d8-d15 (los callee-saved de AAPCS), y al saltar los restaura. El contexto vive
   en el host, no en la memoria GOAL.
2. **`g2c_run_on_stack(sp, fn, args…)`**: cambia sp a una dirección dada, llama a `fn` y, si `fn`
   vuelve, regresa al llamante con su sp original. Es la base de `reset-and-call`, del arranque de
   hilos y del salto de `enter-state`.
3. **Copia de pila del suspend**: la que ya funciona en el PC. Al suspender se copia
   `[sp, stack-top)` a un búfer del host y al reanudar se devuelve a la **misma dirección**, así
   los punteros a la pila siguen valiendo. Incluye el área {marca, id, cookie} en el hilo y el
   gancho de `deactivate`.

### Las 11 funciones

| Función | Implementación nativa |
|---|---|
| `thread-resume` | Guarda el contexto del kernel (`g2c_ctx_save`) en el hilo. Si el hilo arranca, salta a `set-to-run-bootstrap` sobre su pila. Si estaba suspendido, devuelve su pila (copia) y salta a su contexto guardado. |
| `thread-suspend` | Guarda el contexto del hilo, copia su pila fuera y salta al contexto del kernel. Al reanudar continúa como si `suspend` hubiera vuelto; pp sale del proceso, como en el PC. |
| `return-from-thread` | `g2c_ctx_jump` al contexto del kernel que guardó `thread-resume` / `reset-and-call`. |
| `return-from-thread-dead` | Igual, tras marcar el proceso para desactivarlo (lo que hace la versión x86). |
| `reset-and-call` | Guarda el contexto del kernel y hace `g2c_run_on_stack(stack-top del hilo, fn)`. |
| `set-to-run-bootstrap` | En C: toma la función y los argumentos de rreg del hilo, la llama y al volver hace `return-from-thread-dead`. |
| `new` de catch-frame | En C con `g2c_ctx_save`: enlaza el frame en `stack-frame-top`, llama a la función, lo desenlaza y devuelve. El contexto se guarda en el host, indexado por la dirección del frame. |
| `throw-dispatch` | Busca el contexto del frame y hace `g2c_ctx_jump` con el valor lanzado. Los frames intermedios de C se abandonan; el C generado no tiene destructores, así que no pasa nada. |
| `enter-state` (salto) | goal2c reconoce el patrón y emite una llamada a `g2c_rt_state_jump(code, args)`: `g2c_run_on_stack(stack-top del hilo principal, code)` y, si el código vuelve, `return-from-thread-dead`. |
| `deactivate` (autodestrucción) | goal2c reconoce `.push return-from-thread; .ret` y emite `g2c_rt_return_from_thread()`. El resto de la función se traduce normal. |
| `compute-alignment!` | El mismo patrón que `enter-state`: tras el cambio en goal2c se traduce entera. |

Al cargar los objetos, el runtime de la Vita registra estas funciones con su nombre: escribe el
puntero nativo en el objeto función o en la tabla de métodos, como ya hace con el C traducido.
goal2c deja de marcarlas como "sin traducir".

## Cómo se valida (antes de tocar la Vita)

1. **En el PC primero, en el modo mixto:** sustituir las 8 asm-func x86 por estas versiones
   nativas (para x86-64; ya hay dos hechas) y pasar huella, colisión, cámara y el recorrido
   oficial, x86 frente a "todo nativo". Así se valida el diseño con las herramientas que ya
   tenemos.
2. **Port a ARM:** solo cambian las primitivas en ensamblador. Una app de prueba del kernel en
   Vita3K (crear procesos, suspender, `go`, `throw`, matar procesos, reubicar) compara su traza con
   la del PC.
3. Después, el runtime completo en VitaSDK hasta la carga de nivel (resto de la Fase 3).

## Decisiones que necesito de ti

1. **Copia de pila (como en el PC) o pila nativa por proceso en la Vita.**
   - **Copia de pila:** ya validada; usa poca RAM. Cuesta un memcpy por proceso y fotograma: unos
     300 procesos × 1-2 KB = 0,3-0,6 MB por fotograma, del orden de 1 ms en la Vita.
   - **Pila por proceso:** sin copias. Unos 300 × 16 KB ≈ 5 MB de RAM, que la Vita sí puede
     pagar. El cambio de proceso pasa a ser solo un cambio de contexto. Pero es un modelo distinto
     del original: hay que cuidar los hilos trans/post y la reubicación de procesos.
   - **Recomiendo** empezar con la copia (ya probada) y medir. Si el perfil en la Vita lo pide,
     pasar a pilas propias.
2. **Cómo tratar `enter-state`, `deactivate` y `compute-alignment!`.**
   - **(a) Recomendado:** goal2c reconoce los dos patrones (salto con pila nueva y "push + ret")
     y emite llamadas al runtime. No toca goal_src.
   - **(b)** Añadir en goal_src una rama `#cond` para goal2c (CAMBIO JAK VITA en 3 sitios).
3. **setjmp/longjmp de newlib o primitivas propias.** Recomiendo **propias**: guardan
   exactamente los registros de AAPCS (incluidos d8-d15) y se pueden reutilizar igual en el PC
   (x86-64) para validar.
