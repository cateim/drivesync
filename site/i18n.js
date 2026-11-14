// Language for both pages, the same way the tasmota-kincony page does it. The
// English lives in the HTML; the Portuguese is below, keyed by each element's
// data-i18n (its HTML), data-i18n-content (a meta's content) or data-i18n-alt
// (an image's alt). The visitor's own choice wins, then the browser's: any
// Portuguese gets pt-BR, anything else English. A key missing here stays in
// English and says so in the console, so a change to the English text changes
// its line here in the same commit.
const PT = {
  // ---- Both pages ----
  "nav.privacy": "Privacidade",
  "nav.home": "Início",
  "foot.text": `
          O DriveSync é software livre sob a licença MIT, feito por
          <a href="https://github.com/CaTeIM">Gustavo Cateim</a>.
          <a href="privacy.html">Política de privacidade</a>.
          <a href="terms.html">Termos de serviço</a>. Sem vínculo com o Google. Google Drive é
          uma marca do Google LLC.`,

  // ---- Home ----
  "home.title": "DriveSync",
  "home.description":
    "O DriveSync mantém uma pasta do seu computador com Omarchy e o seu Google Drive sincronizados nos dois sentidos, e mostra como essa sincronização está indo.",
  "home.h1": "Google Drive no Omarchy, sincronizado e à vista",
  "home.sub": `
        O DriveSync é um app de desktop para o <a href="https://omarchy.org/">Omarchy</a> que
        mantém uma pasta do seu computador (<code>~/Drive</code>) e o seu Google Drive
        sincronizados nos dois sentidos, a cada 15 minutos, com o
        <a href="https://rclone.org/bisync/">rclone bisync</a>, e mostra como essa sincronização
        está indo: numa janela, na barra do Omarchy e pela linha de comando.`,
  "home.shotAlt":
    "A janela do DriveSync durante uma rodada: o título Sincronizando com uma barra de progresso, as rodadas recentes e os arquivos que a rodada escolhida copiou",
  "home.whatTitle": "O que ele faz",
  "home.what1": `
          <strong>Estado</strong>: sincronizado, sincronizando agora, pausado, ou com falha, com
          a causa e o caminho para resolver.`,
  "home.what2": `
          <strong>Rodadas e conflitos</strong>: o que cada rodada mudou na nuvem e no computador,
          arquivo por arquivo, e as cópias de conflito que ficam quando um arquivo mudou dos dois
          lados.`,
  "home.what3": `
          <strong>Widget na barra e notificações</strong>: uma nuvem na barra do Omarchy com o
          mesmo estado, e uma notificação quando uma rodada falha e quando volta a funcionar.`,
  "home.what4": `
          <strong>Controles</strong>: sincronizar agora, pausar e retomar, pelo teclado.`,
  "home.what5": `
          <strong>Segurança</strong>: ele só conserta uma sincronização quebrada depois que você
          confirma um ensaio, e quando arquivos demais seriam apagados ele para e mostra a lista
          antes.`,
  "home.googleTitle": "A sua conta Google",
  "home.google1": `
          Para manter a pasta e o seu Google Drive iguais, o DriveSync pede ao Google acesso ao
          seu Drive inteiro: ele lista, baixa, envia, altera e apaga arquivos para que as duas
          cópias fiquem iguais. Os seus arquivos só trafegam entre o seu computador e o Google. O
          desenvolvedor não tem servidor nenhum e não recebe nenhum dado seu, e o app não tem
          estatísticas de uso.`,
  "home.google2": `
          Leia a <a href="privacy.html">política de privacidade</a> para os detalhes e para saber
          como retirar o acesso a qualquer momento.`,
  "home.installTitle": "Instalação",
  "home.install1": `
        O DriveSync roda no Omarchy (Arch Linux e Hyprland) e é compilado a partir do código:`,
  "home.install2": `
        O comando compila o app, empacota e instala com o pacman. Depois disso o DriveSync
        aparece no lançador do Omarchy. O código, a licença (MIT) e o manual completo estão no
        <a href="https://github.com/CaTeIM/drivesync">GitHub</a>.`,

  // ---- Privacy policy ----
  "privacy.title": "Política de privacidade do DriveSync",
  "privacy.description":
    "Como o DriveSync acessa, usa, guarda e compartilha dados de usuário do Google: ele roda no seu computador, e o desenvolvedor não recebe nenhum dado seu.",
  "privacy.h1": "Política de privacidade",
  "privacy.updated": "Atualizada em 14 de novembro de 2026",
  "privacy.intro": `
        O DriveSync é um app de desktop que roda no seu próprio computador. Ele mantém uma pasta
        desse computador e o seu Google Drive sincronizados, usando o
        <a href="https://rclone.org/">rclone</a>. O desenvolvedor não tem servidor nenhum, não
        coleta nada e não recebe nenhum dado seu. Esta página explica o que o DriveSync faz com o
        acesso que você dá a ele na sua conta Google.`,
  "privacy.accessTitle": "O que o DriveSync acessa",
  "privacy.access1": `
        Quando você entra com a sua conta, o Google pede que você deixe o DriveSync ver, editar,
        criar e apagar todos os arquivos do seu Google Drive (o escopo
        <code>https://www.googleapis.com/auth/drive</code>). O DriveSync precisa do Drive inteiro
        porque espelha o Drive numa pasta do seu computador, nos dois sentidos. Ele lê os nomes,
        tamanhos, datas e conteúdos dos arquivos do seu Drive. Ele não pede mais nada da sua
        conta Google.`,
  "privacy.useTitle": "Como o DriveSync usa esse acesso",
  "privacy.use1": `
        Só para manter as duas cópias iguais e para mostrar a você como isso está indo. Em cada
        rodada ele lista os arquivos dos dois lados, copia os arquivos novos e alterados para cada
        lado e apaga de um lado os arquivos que você apagou do outro (os arquivos apagados da
        pasta vão para a lixeira do Drive). Os nomes dos arquivos aparecem na janela do DriveSync,
        nas notificações e num log no seu computador, para você ver o que cada rodada fez. Ele não
        faz mais nada com os seus dados: nada de publicidade, nada de perfil, e nenhum treino de
        modelos de IA ou de aprendizado de máquina.`,
  "privacy.storeTitle": "Onde os seus dados ficam",
  "privacy.store1": `
          Os seus arquivos: na pasta que você escolheu (<code>~/Drive</code> por padrão) e no seu
          Google Drive. Eles só trafegam entre o seu computador e os servidores do Google, por
          HTTPS.`,
  "privacy.store2": `
          O token de acesso que o Google devolve: no arquivo de configuração do rclone no seu
          computador (por padrão <code>~/.config/rclone/rclone.conf</code>).`,
  "privacy.store3": `
          O log da sincronização, com os nomes dos arquivos: <code>~/.local/state/drive-sync.log</code>.
          As configurações e o estado do DriveSync: <code>~/.config/drive-sync/</code> e
          <code>~/.local/state/drive-sync-app/</code>.`,
  "privacy.store4": `
        Tudo isso fica no seu computador até você apagar. O desenvolvedor não tem cópia de nada
        disso, então não há nada para guardar ou apagar do lado dele.`,
  "privacy.shareTitle": "Compartilhamento",
  "privacy.share1": `
        O DriveSync não envia os seus dados, os nomes dos seus arquivos nem nada sobre o seu uso
        do app para o desenvolvedor ou para mais ninguém. Ele não tem estatísticas de uso,
        telemetria nem relatório de falhas, e não vende nem compartilha dados. A única parte com
        quem ele fala é o Google, para ler e gravar o seu próprio Drive, e a
        <a href="https://policies.google.com/privacy">política de privacidade</a> do próprio
        Google cobre isso.`,
  "privacy.limitedTitle": "Política de Dados do Usuário dos Serviços de API do Google",
  "privacy.limited1": `
        O uso e a transferência para qualquer outro app, pelo DriveSync, de informações recebidas
        das APIs do Google seguem a
        <a href="https://developers.google.com/terms/api-services-user-data-policy"
          >Política de Dados do Usuário dos Serviços de API do Google</a
        >, incluindo os requisitos de Uso Limitado.`,
  "privacy.controlTitle": "Como retirar o acesso",
  "privacy.control1": `
          Retire o acesso do DriveSync a qualquer momento na página de
          <a href="https://myaccount.google.com/connections">conexões com terceiros</a> da sua
          conta Google. A sincronização para na rodada seguinte.`,
  "privacy.control2": `
          Apague o token do seu computador com <code>rclone config delete</code> seguido do nome
          do remote do Drive.`,
  "privacy.control3": `
          Desinstale o app com <code>sudo pacman -R drive-sync</code>, e apague as pastas listadas
          acima se quiser que elas sumam também. Os seus arquivos continuam na pasta e no seu
          Drive.`,
  "privacy.changesTitle": "Mudanças e contato",
  "privacy.changes1": `
        Uma mudança nesta política é publicada nesta página com uma data nova, e o histórico fica
        no <a href="https://github.com/CaTeIM/drivesync">repositório do código</a>. Dúvidas vão
        para o <a href="https://github.com/CaTeIM/drivesync/issues">rastreador de issues</a> do
        projeto.`,

  // ---- Terms of service ----
  "terms.title": "Termos de serviço do DriveSync",
  "terms.description":
    "Os termos de uso do DriveSync, um app de desktop gratuito que mantém uma pasta e o seu Google Drive sincronizados.",
  "terms.h1": "Termos de serviço",
  "terms.updated": "Atualizados em 14 de novembro de 2026",
  "terms.intro": `
        Estes termos cobrem o uso do DriveSync, o app de desktop, e deste site. Ao usar o
        DriveSync você concorda com eles.`,
  "terms.softwareTitle": "O software",
  "terms.software1": `
        O DriveSync é software livre e de código aberto sob a
        <a href="https://github.com/CaTeIM/drivesync/blob/master/LICENSE">licença MIT</a>, que é
        o que rege copiar, alterar e compartilhar o programa. Ele não custa nada, não precisa de
        conta com o desenvolvedor e roda inteiro no seu computador.`,
  "terms.googleTitle": "A sua conta Google",
  "terms.google1": `
        O DriveSync trabalha no seu Google Drive com o acesso que você dá a ele quando entra com a
        sua conta, e só em seu nome. O seu uso do Google Drive continua sob os
        <a href="https://policies.google.com/terms">Termos de Serviço do Google</a>, e você é
        responsável pelo que guarda e sincroniza lá. Você pode retirar o acesso do DriveSync a
        qualquer momento, como explica a <a href="privacy.html">política de privacidade</a>.`,
  "terms.syncTitle": "O que é uma sincronização nos dois sentidos",
  "terms.sync1": `
        O DriveSync copia as mudanças nos dois sentidos: um arquivo que você altera ou apaga de um
        lado é alterado ou apagado do outro. Ele para e pergunta antes de uma rodada que apagaria
        arquivos demais, e guarda uma cópia de conflito quando um arquivo mudou dos dois lados,
        mas não consegue proteger contra todo erro ou falha. Mantenha um backup separado de
        qualquer arquivo que você não pode perder.`,
  "terms.warrantyTitle": "Sem garantia",
  "terms.warranty1": `
        O DriveSync é fornecido "no estado em que se encontra", sem garantia de nenhum tipo. Até
        onde a lei permite, o desenvolvedor não responde por perda de dados ou outro dano que
        venha do uso dele, como diz a licença MIT.`,
  "terms.changesTitle": "Mudanças e contato",
  "terms.changes1": `
        Uma mudança nestes termos é publicada nesta página com uma data nova, e o histórico fica
        no <a href="https://github.com/CaTeIM/drivesync">repositório do código</a>. Dúvidas vão
        para o <a href="https://github.com/CaTeIM/drivesync/issues">rastreador de issues</a> do
        projeto.`,
};

(() => {
  let lang = null;
  try {
    lang = localStorage.getItem("lang");
  } catch {}
  if (lang !== "en" && lang !== "pt-BR") {
    const nav = (navigator.languages && navigator.languages[0]) || navigator.language || "";
    lang = /^pt(-|$)/i.test(nav) ? "pt-BR" : "en";
  }

  // Every translatable spot with its English, read once from the page.
  const spots = [];
  for (const [attr, prop] of [
    ["data-i18n", null],
    ["data-i18n-content", "content"],
    ["data-i18n-alt", "alt"],
  ]) {
    document.querySelectorAll(`[${attr}]`).forEach((el) => {
      const english = prop ? el.getAttribute(prop) : el.innerHTML;
      spots.push({ el, key: el.getAttribute(attr), prop, english });
    });
  }

  function setLang(next, remember) {
    document.documentElement.lang = next;
    for (const { el, key, prop, english } of spots) {
      if (next === "pt-BR" && !PT[key]) console.warn("i18n: no pt-BR text for", key);
      const text = next === "pt-BR" && PT[key] ? PT[key] : english;
      if (prop) el.setAttribute(prop, text);
      else el.innerHTML = text;
    }
    document.querySelectorAll(".lang button").forEach((button) => {
      button.setAttribute("aria-pressed", String(button.dataset.lang === next));
    });
    if (remember) {
      try {
        localStorage.setItem("lang", next);
      } catch {}
    }
  }

  document.querySelectorAll(".lang button").forEach((button) => {
    button.addEventListener("click", () => setLang(button.dataset.lang, true));
  });
  setLang(lang, false);
})();
