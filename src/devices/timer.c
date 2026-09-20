#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"

//struct que vai guardar as threads que vao entrar em sleep
struct threads_dormindo{
  struct list_elem elemento;
  int64_t hora_de_acordar;
  struct semaphore semaforo;
};

//uso static porque so vai ser usado nesse arquivo
static struct list lista_de_dormindo;
  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void) 
{
  //a funcao timer_interrupt e pode ser chamada a qualquer momento, entao a lista deve ser inicializada antes de qualquer coisa
  list_init (&lista_de_dormindo);
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
//funcao alvo a emplementacao
void
timer_sleep (int64_t ticks) 
{
  //representa a thread que vai ser colocada para dormir
  struct threads_dormindo thread_dormindo;
  int64_t tempo_total;
  int64_t start = timer_ticks ();

  ASSERT (intr_get_level () == INTR_ON); 

  tempo_total = start + ticks;
  //desativa as interrupcoes para evitar que a thread seja acordada antes do tempo
  enum intr_level old_level = intr_disable ();

  //vai dar o tempo que a thread precisa acordar, como se colocasse um "depertador"
  thread_dormindo.hora_de_acordar = tempo_total;
  sema_init(&thread_dormindo.semaforo, 0);

  //vou chamar a funcao list_push_back para colocar a thread no final da lista de threads que vao dormir
  list_push_back(&lista_de_dormindo, &thread_dormindo.elemento);
  sema_down(&thread_dormindo.semaforo);
  //restaura o estado original das interrupcoes, para que a thread possa ser acordada
  intr_set_level (old_level);
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
//segunda funcao alvo, responsavel por acordar as threads que estao dormindo
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++;
  //escrevo o codigo aqui no meio, porque o thread ticks vai ser responsavel por designar a funcao da thread apois ela acordar, e o ticks anterior e oquer vai atualizar o estado do sistema
 
  struct list_elem *aux;
  //checa se a lista esta vazia para evitar erro de segmentacao, se nao estiver vazia, pega o primeiro elemento da lista
  if (!list_empty(&lista_de_dormindo)) {
    aux = list_front(&lista_de_dormindo);
  }else{
    aux = list_end(&lista_de_dormindo);
  }
  struct list_elem *aux2;
  //uso list_empty para verificar se a lista esta vazia, ou seja o while vai rodar enquato tiver elementos na lista
  while(aux != list_end(&lista_de_dormindo)){
    //o list_entry vai pegar o elemento da lista e transformar em uma struct threads_dormindo (reconstitui a struct), e ai eu consigo acessar o campo hora_de_acordar
    //salva isso numa variavel e comparar com a variavel ticks, que representa o tempo atual do sistema
    struct threads_dormindo *thread_atual = list_entry (aux, struct threads_dormindo, elemento);
    if(thread_atual->hora_de_acordar <= ticks){
      //se a hora de acordar for menor ou igual ao ticks atual, entao a thread vai acordar
      sema_up(&thread_atual->semaforo);
      //importante remover a thread da lista
      aux2 = aux;
      aux = list_next(aux);
      list_remove(aux2);
    }else{
    aux = list_next(aux);
    }
  }
  thread_tick ();
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}
