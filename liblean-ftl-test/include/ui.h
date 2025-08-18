#pragma once

void ui_show_idle(){
  led1(1);
}
void ui_show_busy(){
  led1(0);
}
#define DUTY_CYCLE_25 4
#define DUTY_CYCLE_50 8
#define DUTY_CYCLE_75 12
bool ui_led1_blink_ms(unsigned int total_ms, unsigned int duty_cycle_0_to_15){
  if(total_ms<100) throw_exception(INTERNAL_ERROR_CORRUPT);
  const unsigned int base_ms = total_ms/16;
  const unsigned int high_ms = base_ms * duty_cycle_0_to_15;
  const unsigned int low_ms = total_ms - high_ms;
  unsigned int b = 0;
  led1(1);
  for(unsigned int i=0;i<4;i++){
    delay_ms(high_ms/4);
    #ifdef HAS_SAFE_BUTTON
    if(button()) return 1;
    #else
    if(button()) b++; else if(b<3) b = 0;
    #endif
  }
  led1(0);
  for(unsigned int i=0;i<4;i++){
    delay_ms(low_ms/4);
    #ifdef HAS_SAFE_BUTTON
    if(button()) return 1;
    #else
    if(button()) b++; else if(b<3) b = 0;
    #endif
  }
  return b>=3;
}
void ui_wait_button(){
  if(button()) throw_exception(ERROR_BUTTON_PRESSED);
  while(!ui_led1_blink_ms(200,DUTY_CYCLE_50));//wait user press the button
  while(1){//wait user release the button
    while(button());
    #ifdef HAS_SAFE_BUTTON
    break;
    #else
    delay_ms(200);//wait and check again for robustness against glitch
    if(!button()) break;
    #endif
  }
}
