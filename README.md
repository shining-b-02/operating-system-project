2025년 2학기 숭실대학교 컴퓨터학부 운영체제 (홍지만 교수님) 설계 과제 

평점 A+


리포지토리 구조
* README.md: 과제 명세 요약
* .docx: 보고서(과제 개요, 상세 설계, 소스 코드 수정 내역)
* 소스코드: 기존 xv6에서 변경된 파일


설계 과제 요약 

과제 #0 : Linux의 명령어 실행을 통한 운영체제 기본 개념 이해

과제 #1 : xv6 설치 및 system calls in xv6
    * xv6을 설치‧컴파일하고, 사용자 프로그램 helloxv6, psinfo를 작성하며, 시스템 콜 hello_number(int), get_procinfo(int, struct procinfo*)를 추가해 쉘에서 동작을 검증한다.

과제 #2 : Stride Scheduling in xv6
    * 교육용 운영체제 xv6의 기본 스케줄러를 Stride 스케줄러로 교체하고, 관련 시스템콜과 디버깅 출력을 구현하여 스케줄링 동작을 가시화하는 것이다. 구현은 기존 xv6 코어 구조를 유지하되, 한 틱 단위의 프리엠션과 공정성을 보장하는 pass/stride 회계(accounting)와 오버플로 방지(rebase) 로직을 추가하는 데 초점을 둔다. 

과제 #3 : Physical Page Frame Tracking in xv6
    * xv6에 소프트웨어 페이지 워커(sw_vtop), 물리 프레임 사용 현황 계측(PF 테이블·memdump), 역페이지 테이블(IPT)·소프트 TLB(STLB)를 확장·구현하는 것이다. sw_vtop은 커널이 PDE/PTE를 직접 파싱해 VA→PA를 계산하며, 사용자 도구 vtop으로 물리주소와 PTE 플래그를 조회한다. PF 테이블은 모든 물리 프레임의 할당 여부·소유 pid·할당 시각을 추적하고 memdump로 덤프하며, memstress로 부하 상황에서 동작을 검증한다. IPT+STLB는 PFN→(pid, va, flags) 역참조와 변환 캐시를 제공하고, 페이지 테이블 변화와 항상 일관성을 유지하도록 갱신한다. 커널 페이지는 추적하지 않으며 사용자 페이지(PTE_U)만 관리한다.

과제 #4 : Snapshot(Checkpointing) in xv6
    * xv6 파일시스템에 시점 스냅샷(snapshot) 기능을 추가한다. 스냅샷은 생성 시점의 루트 트리를 보존하고, 이후 원본 파일에 쓰기가 발생해도 스냅샷의 내용은 Copy-On-Write(COW) 로 보호된다. 또한 임의 시점으로 롤백하고, 불필요해진 스냅샷은 삭제할 수 있다. 

관련 자료

* xv6: https://github.com/mit-pdos/xv6-public
* 교재: https://pages.cs.wisc.edu/~remzi/OSTEP/Korean/
* MIT 6.1810 Operating System Engineering: https://pdos.csail.mit.edu/6.1810/

