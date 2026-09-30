// Copy this file to service_contact.h (same folder as the sketch) and put
// the real service number in it. service_contact.h is in .gitignore, so the
// number stays on the machine that flashes the board and never reaches git.
//
// The fault screen shows this to a customer who has paid and needs help:
//     SORRY - JAMMED
//     07XX XXX XXX
// SIXTEEN CHARACTERS MAXIMUM; the display cuts off anything longer.
// Without service_contact.h the sketch uses this placeholder and the boot
// self-test warns about it.
#define SERVICE_CONTACT_TEXT "07XX XXX XXX"
