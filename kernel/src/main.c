#include <linux/module.h>

#include "capnp_c.h"
#include "addressbook.capnp.h"

static capn_text chars_to_text(const char *chars) {
  return (capn_text) {
    .len = (int) strlen(chars),
    .str = chars,
    .seg = NULL,
  };
}

void __init test_capn(void) {
  uint8_t buf[4096];
  ssize_t sz = 0;

  const char *name = "Firstname Lastname";
  const char *email = "username@domain.com";
  const char *school = "of life";

  {
    struct capn c;
    capn_init_malloc(&c);
    capn_ptr cr = capn_root(&c);
    struct capn_segment *cs = cr.seg;

    // Set initial object in `p`.
    struct Person p = {
      .id = 17,
      .name = chars_to_text(name),
      .email = chars_to_text(email),
    };
    p.employment_which = Person_employment_school;
    p.employment.school = chars_to_text(school);

    p.phones = new_Person_PhoneNumber_list(cs, 2);
    struct Person_PhoneNumber pn0 = {
      .number = chars_to_text("123"),
      .type = Person_PhoneNumber_Type_work,
    };
    set_Person_PhoneNumber(&pn0, p.phones, 0);
    struct Person_PhoneNumber pn1 = {
      .number = chars_to_text("234"),
      .type = Person_PhoneNumber_Type_home,
    };
    set_Person_PhoneNumber(&pn1, p.phones, 1);

    Person_ptr pp = new_Person(cs);
    write_Person(&p, pp);
    int setp_ret = capn_setp(capn_root(&c), 0, pp.p);
    printk("0 == setp_ret = %d\n", 0 == setp_ret);
    sz = capn_write_mem(&c, buf, sizeof(buf), 0 /* packed */);
    capn_free(&c);
  }
}	

static int __init hello_init(void) {
	printk("Entering: %s\n",__FUNCTION__);
	test_capn();
	return 0;
}

static void __exit hello_exit(void) {
	printk(KERN_INFO "exiting hello module\n");
}

module_init(hello_init); 
module_exit(hello_exit);

MODULE_LICENSE("GPL");
